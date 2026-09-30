#include "backend.hpp"
#include "client.hpp"
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtConcurrent>
#include <map>
#include <thread>

using namespace rime_ime;
namespace {
std::filesystem::path localPath(const QUrl &url) {
    if (!url.isLocalFile() || url.toLocalFile().isEmpty())
        throw std::runtime_error("请选择本地文件。");
#ifdef _WIN32
    return url.toLocalFile().toStdWString();
#else
    return url.toLocalFile().toStdString();
#endif
}
QString readDiagnostic(const Paths &paths) {
    try {
        if (!fs::exists(paths.cache / "diagnostic.json"))
            return {};
        auto data = Json::parse(read_file(paths.cache / "diagnostic.json", 64 * 1024));
        const auto message = data.value("message", std::string{});
        return message.empty() ? QString::fromUtf8("专属候选窗运行正常。")
                               : QString::fromStdString(message);
    } catch (const std::exception &) {
        return QString::fromUtf8("候选窗诊断文件无法读取。请重新激活 R-IME 后再检查。");
    }
}
struct OperationResult {
    Json config;
    std::vector<Phrase> phrases;
    QString error, diagnostic;
};
} // namespace

SettingsBackend::SettingsBackend(Paths paths, bool connectEngine, QObject *parent)
    : QObject(parent), paths_(std::move(paths)), config_(default_config()), savedConfig_(config_),
      autoStart_(connectEngine) {
    try {
        config_ = load_config(paths_.config);
        savedConfig_ = config_;
        if (fs::exists(paths_.data / "phrases.tsv"))
            phrases_ = parse_phrases(read_file(paths_.data / "phrases.tsv", 8 * 1024 * 1024));
        savedPhrases_ = phrases_;
    } catch (const std::exception &e) {
        loadFailed_ = true;
        error_ = QString::fromUtf8("无法加载设置或短语；原文件已保留。请恢复有效备份。\n") +
                 QString::fromUtf8(e.what());
    }
    diagnostic_ = readDiagnostic(paths_);
    if (connectEngine && !loadFailed_)
        QTimer::singleShot(0, this, &SettingsBackend::refreshStatus);
}
QVariantMap SettingsBackend::config() const {
    return QJsonDocument::fromJson(QByteArray::fromStdString(config_.dump()))
        .object()
        .toVariantMap();
}
QVariantList SettingsBackend::phrases() const {
    QVariantList result;
    result.reserve(qsizetype(phrases_.size()));
    for (const auto &p : phrases_)
        result.append(QVariantMap{{"code", QString::fromStdString(p.code)},
                                  {"text", QString::fromStdString(p.text)},
                                  {"weight", p.weight}});
    return result;
}
bool SettingsBackend::dirty() const {
    if (repairRequested_)
        return true;
    if (config_ != savedConfig_ || phrases_.size() != savedPhrases_.size())
        return true;
    for (std::size_t i = 0; i < phrases_.size(); ++i)
        if (phrases_[i].code != savedPhrases_[i].code ||
            phrases_[i].text != savedPhrases_[i].text ||
            phrases_[i].weight != savedPhrases_[i].weight)
            return true;
    return false;
}
QVariant SettingsBackend::value(const QString &group, const QString &key) const {
    return config().value(group).toMap().value(key);
}
void SettingsBackend::fail(const std::exception &e) {
    error_ = QString::fromUtf8(e.what());
    emit stateChanged();
}
bool SettingsBackend::setValue(const QString &group, const QString &key, const QVariant &value) {
    if (busy_)
        return false;
    try {
        Json draft = config_;
        auto encoded = QJsonDocument(QJsonObject::fromVariantMap({{"value", value}}))
                           .toJson(QJsonDocument::Compact);
        draft.at(group.toStdString()).at(key.toStdString()) =
            Json::parse(encoded.toStdString()).at("value");
        validate_config(draft);
        config_ = std::move(draft);
        error_.clear();
        status_.clear();
        emit changed();
        emit stateChanged();
        return true;
    } catch (const std::exception &e) {
        fail(e);
        return false;
    }
}
bool SettingsBackend::upsertPhrase(int index, const QString &code, const QString &text,
                                   int weight) {
    if (busy_)
        return false;
    try {
        if (index < -1 || index >= int(phrases_.size()))
            throw std::runtime_error("短语已不存在，请重新选择。");
        auto draft = phrases_;
        Phrase phrase{code.trimmed().toStdString(), text.toStdString(), weight};
        if (index < 0)
            draft.push_back(std::move(phrase));
        else
            draft[std::size_t(index)] = std::move(phrase);
        (void)format_phrases(draft);
        phrases_ = std::move(draft);
        error_.clear();
        status_.clear();
        emit changed();
        emit stateChanged();
        return true;
    } catch (const std::exception &e) {
        fail(e);
        return false;
    }
}
void SettingsBackend::removePhrase(int index) {
    if (busy_ || index < 0 || index >= int(phrases_.size()))
        return;
    phrases_.erase(phrases_.begin() + index);
    error_.clear();
    status_.clear();
    emit changed();
    emit stateChanged();
}
bool SettingsBackend::importPhrases(const QUrl &url) {
    if (busy_)
        return false;
    try {
        auto bytes = read_file(localPath(url), 8 * 1024 * 1024);
        if (bytes.starts_with("\xef\xbb\xbf"))
            bytes.erase(0, 3);
        auto incoming = parse_phrases(bytes);
        auto draft = phrases_;
        std::map<std::pair<std::string, std::string>, std::size_t> index;
        for (std::size_t i = 0; i < draft.size(); ++i)
            index[{draft[i].code, draft[i].text}] = i;
        for (const auto &phrase : incoming) {
            auto key = std::make_pair(phrase.code, phrase.text);
            if (auto it = index.find(key); it != index.end())
                draft[it->second] = phrase;
            else {
                index[key] = draft.size();
                draft.push_back(phrase);
            }
        }
        (void)format_phrases(draft);
        phrases_ = std::move(draft);
        error_.clear();
        status_ = QString::fromUtf8("短语已导入，点击“应用”生效。");
        emit changed();
        emit stateChanged();
        return true;
    } catch (const std::exception &e) {
        fail(e);
        return false;
    }
}
bool SettingsBackend::exportPhrases(const QUrl &url) {
    if (busy_)
        return false;
    try {
        atomic_write(localPath(url), format_phrases(phrases_));
        error_.clear();
        status_ = QString::fromUtf8("短语已导出。");
        emit stateChanged();
        return true;
    } catch (const std::exception &e) {
        fail(e);
        return false;
    }
}
void SettingsBackend::restoreDefaults() {
    if (busy_)
        return;
    config_ = default_config();
    repairRequested_ = loadFailed_;
    error_.clear();
    status_ = QString::fromUtf8("默认设置已准备好，点击“应用”生效。词库将保留。");
    emit changed();
    emit stateChanged();
}
void SettingsBackend::discard() {
    if (busy_)
        return;
    config_ = savedConfig_;
    repairRequested_ = false;
    phrases_ = savedPhrases_;
    error_.clear();
    status_ = QString::fromUtf8("已放弃未应用的更改。");
    emit changed();
    emit stateChanged();
}
void SettingsBackend::apply() {
    if (loadFailed_ && !repairRequested_) {
        error_ = QString::fromUtf8(
            "原设置或短语无法读取。为保留现有数据，请先恢复有效备份，再应用更改。");
        emit stateChanged();
        return;
    }
    run("configure");
}
void SettingsBackend::exportBackup(const QUrl &file) {
    if (dirty()) {
        error_ = QString::fromUtf8("请先应用或放弃更改，再导出完整备份。");
        emit stateChanged();
        return;
    }
    run("backup", file);
}
void SettingsBackend::importBackup(const QUrl &file) {
    run("restore", file);
}
void SettingsBackend::refreshStatus() {
    run("status");
}

void SettingsBackend::run(const QString &operation, const QUrl &file) {
    if (busy_)
        return;
    busy_ = true;
    error_.clear();
    status_ = operation == "configure" ? QString::fromUtf8("正在应用设置并部署词库…")
              : operation == "backup"  ? QString::fromUtf8("正在安全导出词库…")
              : operation == "restore" ? QString::fromUtf8("正在校验并恢复备份…")
                                       : QString::fromUtf8("正在连接输入引擎…");
    emit stateChanged();
    auto *watcher = new QFutureWatcher<OperationResult>(this);
    auto paths = paths_;
    auto config = config_;
    auto phrases = phrases_;
    const bool autoStart = autoStart_;
    const bool repair = repairRequested_ && loadFailed_;
    connect(watcher, &QFutureWatcher<OperationResult>::finished, this, [this, watcher, operation] {
        auto result = watcher->result();
        watcher->deleteLater();
        busy_ = false;
        diagnostic_ = result.diagnostic;
        if (!result.error.isEmpty()) {
            error_ = result.error;
            status_ = QString::fromUtf8("操作未完成。");
        } else {
            if (operation == "configure" || operation == "restore" ||
                (operation == "status" && !dirty())) {
                config_ = result.config;
                savedConfig_ = config_;
                phrases_ = result.phrases;
                savedPhrases_ = phrases_;
                if (operation == "restore" || operation == "status" || operation == "configure")
                    loadFailed_ = false;
                repairRequested_ = false;
                emit changed();
            }
            status_ = operation == "configure" ? QString::fromUtf8("设置已应用。")
                      : operation == "restore" ? QString::fromUtf8("备份已恢复。")
                      : operation == "backup"  ? QString::fromUtf8("备份已导出。")
                                               : QString::fromUtf8("输入引擎已连接。");
        }
        emit stateChanged();
    });
    watcher->setFuture(QtConcurrent::run([paths, config, phrases, autoStart, operation, file,
                                          repair] {
        OperationResult result;
        try {
            Client client(paths.runtime / "engine.sock");
            Json status;
            try {
                status = client.call({{"op", "status"}}, 1500);
            } catch (const RpcError &) {
                throw;
            } catch (const std::exception &) {
                if (!autoStart || !start_engine())
                    throw std::runtime_error(
                        "无法连接输入引擎。请检查 r-ime-engine 是否已安装，并在桌面启用 R-IME。");
                // Cold dictionary deployment can take two minutes. The GUI stays responsive;
                // mutating requests are never replayed after a lost response.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(130);
                for (;;) {
                    try {
                        status = client.call({{"op", "status"}}, 1500);
                        break;
                    } catch (const RpcError &) {
                        throw;
                    } catch (const std::exception &) {
                        if (std::chrono::steady_clock::now() >= deadline)
                            throw;
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }
            }
            if (operation != "restore" && !(operation == "configure" && repair) &&
                status.value("recovery_required", false))
                throw std::runtime_error(
                    status.value("diagnostic", std::string("请从有效备份恢复配置。")));
            if (operation == "configure") {
                validate_config(config);
                Json request = {{"op", "configure"}, {"config", config}};
                if (repair)
                    request["repair_config"] = true;
                else
                    request["phrases"] = format_phrases(phrases);
                auto reply = client.call(request, 180000);
                result.config = reply.at("config");
                result.phrases = phrases;
                if (repair && fs::exists(paths.data / "phrases.tsv"))
                    result.phrases = parse_phrases(read_file(paths.data / "phrases.tsv"));
            } else if (operation == "backup") {
                auto path = localPath(file);
                auto reply = client.call({{"op", "backup"}}, 180000);
                atomic_write(path, reply.at("backup").dump(2) + "\n");
            } else if (operation == "restore") {
                auto archive = Json::parse(read_file(localPath(file), 136 * 1024 * 1024));
                auto reply = client.call({{"op", "restore"}, {"backup", archive}}, 180000);
                result.config = reply.at("config");
                if (fs::exists(paths.data / "phrases.tsv"))
                    result.phrases =
                        parse_phrases(read_file(paths.data / "phrases.tsv", 8 * 1024 * 1024));
            } else {
                result.config = status.at("config");
                if (fs::exists(paths.data / "phrases.tsv"))
                    result.phrases =
                        parse_phrases(read_file(paths.data / "phrases.tsv", 8 * 1024 * 1024));
            }
            if (operation != "backup")
                validate_config(result.config);
        } catch (const std::exception &e) {
            result.error = QString::fromUtf8(e.what());
        }
        result.diagnostic = readDiagnostic(paths);
        return result;
    }));
}
