#pragma once
#include "rime_ime/config.hpp"
#include <QObject>
#include <QUrl>
#include <QVariant>

class SettingsBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap config READ config NOTIFY changed)
    Q_PROPERTY(QVariantList phrases READ phrases NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY stateChanged)
  public:
    explicit SettingsBackend(rime_ime::Paths paths = rime_ime::Paths::from_environment(),
                             bool connectEngine = true, QObject *parent = nullptr);
    QVariantMap config() const;
    QVariantList phrases() const;
    bool dirty() const;
    bool busy() const {
        return busy_;
    }
    QString error() const {
        return error_;
    }
    QString status() const {
        return status_;
    }
    QString diagnostic() const {
        return diagnostic_;
    }
    Q_INVOKABLE QVariant value(const QString &group, const QString &key) const;
    Q_INVOKABLE bool setValue(const QString &group, const QString &key, const QVariant &value);
    Q_INVOKABLE bool upsertPhrase(int index, const QString &code, const QString &text, int weight);
    Q_INVOKABLE void removePhrase(int index);
    Q_INVOKABLE bool importPhrases(const QUrl &url);
    Q_INVOKABLE bool exportPhrases(const QUrl &url);
    Q_INVOKABLE void restoreDefaults();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void exportBackup(const QUrl &url);
    Q_INVOKABLE void importBackup(const QUrl &url);
    Q_INVOKABLE void refreshStatus();
  signals:
    void changed();
    void stateChanged();

  private:
    rime_ime::Paths paths_;
    rime_ime::Json config_, savedConfig_;
    std::vector<rime_ime::Phrase> phrases_, savedPhrases_;
    bool busy_ = false;
    bool autoStart_ = true, loadFailed_ = false, repairRequested_ = false;
    QString error_, status_, diagnostic_;
    void fail(const std::exception &e);
    void run(const QString &operation, const QUrl &file = {});
};
