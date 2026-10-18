#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>

// One controller per native window. Geometry changes never recreate materials.
class WindowEffects final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool highContrast READ highContrast NOTIFY changed)
    Q_PROPERTY(int cornerRadius READ cornerRadius NOTIFY changed)
public:
    explicit WindowEffects(QObject *parent = nullptr, bool preview = false);
    void setWindow(QWindow *window);
    bool enabled() const { return m_enabled; }
    bool active() const { return m_active; }
    bool highContrast() const { return m_highContrast; }
    int cornerRadius() const;
    Q_INVOKABLE void setEnabled(bool enabled);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reconfigure();
    Q_INVOKABLE void titlebarDoubleClick();
signals:
    void changed();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QPointer<QWindow> m_window;
    bool m_preview = false;
    bool m_enabled = true;
    bool m_active = false;
    bool m_highContrast = false;
    bool m_cached = false;
    bool m_allowed = false;
    int m_radius = 10;
};
