// src/screens/agent_config/AgentChatPanel.h
#pragma once
#include "services/agents/AgentTypes.h"

#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace fincept::screens {

/// Rich chat panel — conversational interface for interacting with configured agents.
/// Matches the AiChatScreen design: proper bubbles, typing indicator, welcome panel,
/// LLM config status in header, QTextEdit multi-line input with Shift+Enter support.
class AgentChatPanel : public QWidget {
    Q_OBJECT
  public:
    explicit AgentChatPanel(QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

  private:
    void build_ui();
    void setup_connections();

    /// Re-apply tr() lookups to every widget whose text we keep a handle to.
    /// Called from changeEvent() on QEvent::LanguageChange.
    void retranslateUi();

    // Portfolio context helpers
    QString build_portfolio_context() const; // returns enriched context string
    void refresh_portfolios();

    // Message area
    void add_user_bubble(const QString& text);
    void add_assistant_bubble(const QString& text, const QString& agent_name = {});
    void add_system_bubble(const QString& text);
    QTextEdit* add_streaming_bubble(const QString& agent_name = {}); // returns live QTextEdit*
    void scroll_to_bottom();
    void clear_chat();

    // State helpers
    void set_executing(bool on);
    void show_welcome(bool on);
    void show_typing(bool on);
    void update_llm_status();

    void send_message();

    // Dedicated read-only E015 bridge. It never enters AgentService's execution path.
    bool e015_selected() const;
    bool e015_active() const;
    bool e015_configured() const;
    void ensure_e015_selector();
    void update_e015_controls();
    void sync_e015_polling();
    void stop_e015_requests(bool preserve_manual = false);
    void check_e015_version(bool manual = false);
    void request_e015_plan(bool automatic);
    void poll_e015_task();
    void control_e015_task(bool resume);
    bool release_e015_unsubmitted_cancel(const QJsonObject& task);
    void display_e015_task(const QJsonObject& task);
    void finish_e015_task(const QJsonObject& plan);
    void submit_e015_manual(const QString& text);
    void fail_e015_manual(const QString& text);
    void reveal_e015_plan();
    void show_e015_status(const QString& text, bool invalidate = false);
    void display_e015_plan(const QJsonObject& plan);
    QNetworkReply* start_e015_request(const QString& path, const QByteArray& body = {}, bool manual = false);

    // ── UI ─────────────────────────────────────────────────────────────────────
    // Header
    QLabel* header_title_ = nullptr;   // "AGENT CHAT"
    QLabel* agent_caption_ = nullptr;  // "AGENT:" caption
    QLabel* hdr_model_lbl_ = nullptr;  // active model pill
    QLabel* hdr_status_lbl_ = nullptr; // Ready / Streaming…
    QLabel* hdr_agent_lbl_ = nullptr;  // selected agent badge
    QComboBox* agent_selector_ = nullptr;
    QPushButton* route_toggle_ = nullptr;
    QPushButton* run_as_task_toggle_ = nullptr; // visible only when agentic_mode_enabled
    QPushButton* clear_btn_ = nullptr;

    // Messages
    QScrollArea* scroll_area_ = nullptr;
    QWidget* messages_container_ = nullptr;
    QVBoxLayout* messages_layout_ = nullptr;
    QWidget* welcome_panel_ = nullptr;
    QLabel* welcome_title_ = nullptr;
    QLabel* welcome_subtitle_ = nullptr;
    QWidget* typing_indicator_ = nullptr;
    QLabel* typing_dots_lbl_ = nullptr;

    // Portfolio context bar
    QLabel* portfolio_caption_ = nullptr; // "PORTFOLIO:" caption
    QComboBox* portfolio_combo_ = nullptr;
    QPushButton* analyze_btn_ = nullptr;
    QPushButton* rebalance_btn_ = nullptr;
    QPushButton* risk_btn_ = nullptr;

    // Input
    QTextEdit* input_edit_ = nullptr;
    QPushButton* send_btn_ = nullptr;

    // Status bar
    QLabel* status_label_ = nullptr;

    // ── State ──────────────────────────────────────────────────────────────────
    bool auto_routing_ = false;
    bool run_as_task_ = false;
    bool executing_ = false;
    bool data_loaded_ = false;
    int typing_step_ = 0;
    QString last_query_;
    QString pending_request_id_;
    QString streaming_text_;
    QPointer<QTextEdit> streaming_bubble_widget_;

    QTimer* typing_timer_ = nullptr;
    QTimer* e015_poll_timer_ = nullptr;
    QTimer* e015_task_timer_ = nullptr;
    QNetworkAccessManager* e015_network_ = nullptr;
    QPointer<QNetworkReply> e015_version_reply_;
    QPointer<QNetworkReply> e015_plan_reply_;
    QPointer<QNetworkReply> e015_control_reply_;
    QPointer<QWidget> e015_plan_panel_;
    QPointer<QLabel> e015_plan_status_;
    QPointer<QTextEdit> e015_plan_body_;
    QPointer<QTextEdit> e015_manual_body_;
    QPointer<QPushButton> e015_cancel_btn_;
    QPointer<QPushButton> e015_resume_btn_;
    QString e015_task_id_;
    QString e015_task_status_;
    QString e015_task_version_;
    QString e015_task_request_id_;
    QByteArray e015_task_create_body_;
    QString e015_pending_prompt_;
    int e015_task_control_attempts_ = 0;
    int e015_task_create_attempts_ = 0;
    qint64 e015_task_started_ = 0;
    int e015_task_poll_failures_ = 0;
    quint64 e015_task_epoch_ = 0;
    bool e015_task_automatic_ = false;
    bool e015_task_cancel_requested_ = false;
    bool e015_task_discard_ = false;
    bool e015_task_uncertain_ = false;
    bool e015_task_recovery_requested_ = false;
    QString e015_version_;
    QString e015_generated_at_;
    QString e015_attempted_version_;
    QString e015_manual_prompt_;
    QJsonObject e015_last_plan_;
    bool e015_version_unverified_ = false;
    bool e015_refresh_deferred_ = false;
    bool e015_manual_stale_retried_ = false;
};

} // namespace fincept::screens
