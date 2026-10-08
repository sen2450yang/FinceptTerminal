// src/screens/agent_config/AgentChatPanel.cpp
#include "screens/agent_config/AgentChatPanel.h"

#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "services/agents/AgentService.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/repositories/PortfolioRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/markdown/MarkdownRenderer.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QCompleter>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextOption>
#include <memory>

namespace fincept::screens {

namespace col = fincept::ui::colors;
namespace fnt = fincept::ui::fonts;

static const QString kE015Agent = QStringLiteral("e015-strategy-brain");
static constexpr qint64 kE015ResponseLimit = 2 * 1024 * 1024;

// Escape raw HTML before the existing renderer, and remove Markdown images so
// bridge text cannot request external or local-file resources through QTextEdit.
static QString e015_markdown(const QString& text) {
    auto html = ui::MarkdownRenderer::render(text.toHtmlEscaped());
    html.remove(QRegularExpression(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption));
    html.replace(QStringLiteral("font-size:14px; margin:0"), QStringLiteral("font-size:16px; margin:0"));
    return html;
}

// ── Style helpers ─────────────────────────────────────────────────────────────

static QString bubble_style(const QString& role) {
    if (role == "user")
        return "background:rgba(120,53,15,0.45);border:1px solid rgba(217,119,6,0.28);"
               "border-radius:6px;padding:10px 14px;";
    if (role == "system")
        return QString("background:%1;border:1px solid %2;"
                       "border-radius:6px;padding:10px 14px;")
            .arg(col::BG_SURFACE(), col::BORDER_DIM());
    return QString("background:%1;border:1px solid %2;border-radius:6px;padding:10px 14px;")
        .arg(col::BG_RAISED(), col::BORDER_MED());
}

static QString body_color(const QString& role) {
    if (role == "user")
        return "#fff7ed";
    if (role == "system")
        return col::TEXT_SECONDARY;
    return col::TEXT_PRIMARY;
}

static QString role_color(const QString& role) {
    if (role == "user")
        return col::AMBER;
    if (role == "system")
        return col::TEXT_TERTIARY;
    return col::CYAN;
}

static QString role_label(const QString& role) {
    if (role == "user")
        return QCoreApplication::translate("AgentChatPanel", "You");
    if (role == "system")
        return QCoreApplication::translate("AgentChatPanel", "System");
    return QCoreApplication::translate("AgentChatPanel", "Agent");
}

// ── Constructor ───────────────────────────────────────────────────────────────

AgentChatPanel::AgentChatPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("AgentChatPanel");

    typing_timer_ = new QTimer(this);
    typing_timer_->setInterval(400);
    connect(typing_timer_, &QTimer::timeout, this, [this]() {
        const QStringList states = {tr("Agent is thinking"), tr("Agent is thinking."), tr("Agent is thinking.."),
                                    tr("Agent is thinking...")};
        typing_step_ = (typing_step_ + 1) % states.size();
        typing_dots_lbl_->setText(states[typing_step_]);
    });

    build_ui();
    setup_connections();
    e015_network_ = new QNetworkAccessManager(this);
    e015_network_->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    e015_poll_timer_ = new QTimer(this);
    e015_poll_timer_->setInterval(60000);
    connect(e015_poll_timer_, &QTimer::timeout, this, &AgentChatPanel::check_e015_version);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState) { sync_e015_polling(); });

    // Seed agent selector from cache immediately
    const auto cached = services::AgentService::instance().cached_agents();
    if (!cached.isEmpty()) {
        agent_selector_->blockSignals(true);
        agent_selector_->clear();
        agent_selector_->addItem(tr("Default (global LLM)"), QString{});
        ensure_e015_selector();
        for (const auto& a : cached)
            if (a.id != kE015Agent)
                agent_selector_->addItem(QString("[%1] %2").arg(a.category, a.name), a.id);
        agent_selector_->blockSignals(false);
    }

    update_llm_status();

    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this](const ui::ThemeTokens&) { update(); });
}

// ── Build UI ──────────────────────────────────────────────────────────────────

void AgentChatPanel::build_ui() {
    setStyleSheet(QString("background:%1;").arg(col::BG_BASE()));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header ────────────────────────────────────────────────────────────────
    auto* header = new QWidget(this);
    header->setFixedHeight(52);
    header->setStyleSheet(
        QString("background:%1;border-bottom:1px solid %2;").arg(col::BG_RAISED(), col::BORDER_DIM()));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(14, 0, 12, 0);
    hl->setSpacing(10);

    header_title_ = new QLabel(tr("AGENT CHAT"));
    header_title_->setStyleSheet(
        QString("color:%1;font-size:13px;font-weight:700;letter-spacing:1.5px;").arg(col::AMBER()));
    hl->addWidget(header_title_);

    // Thin divider
    auto* div1 = new QFrame;
    div1->setFrameShape(QFrame::VLine);
    div1->setFixedWidth(1);
    div1->setStyleSheet(QString("background:%1;").arg(col::BORDER_DIM()));
    hl->addWidget(div1);

    // Agent selector
    agent_caption_ = new QLabel(tr("AGENT:"));
    agent_caption_->setStyleSheet(
        QString("color:%1;font-size:9px;font-weight:600;letter-spacing:0.5px;").arg(col::TEXT_TERTIARY()));
    hl->addWidget(agent_caption_);

    agent_selector_ = new QComboBox;
    agent_selector_->addItem(tr("Default (global LLM)"), QString{});
    ensure_e015_selector();
    agent_selector_->setMinimumWidth(200);
    agent_selector_->setMaximumWidth(420);
    agent_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    agent_selector_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    agent_selector_->setCursor(Qt::PointingHandCursor);
    agent_selector_->setToolTip(tr("Select a configured agent, or Default to use the global LLM."));

    // Editable + completer gives us an inline search bar inside the dropdown.
    // PopupCompletion shows filtered matches as a popup list while typing.
    agent_selector_->setEditable(true);
    agent_selector_->setInsertPolicy(QComboBox::NoInsert);
    agent_selector_->lineEdit()->setPlaceholderText(tr("Search agent..."));
    agent_selector_->lineEdit()->setClearButtonEnabled(true);
    {
        auto* completer = new QCompleter(agent_selector_->model(), agent_selector_);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setFilterMode(Qt::MatchContains); // match anywhere in name
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setMaxVisibleItems(12);
        completer->popup()->setStyleSheet(
            QString("QAbstractItemView{"
                    "background:%1;color:%2;border:1px solid %3;"
                    "selection-background-color:%4;"
                    "font-size:11px;padding:2px;outline:none;}"
                    "QAbstractItemView::item{padding:4px 10px;min-height:22px;}")
                .arg(col::BG_RAISED(), col::TEXT_PRIMARY(), col::AMBER(), col::AMBER_DIM()));
        agent_selector_->setCompleter(completer);
    }

    agent_selector_->setStyleSheet(
        QString("QComboBox{background:%1;color:%2;border:1px solid %3;padding:3px 10px;"
                "font-size:10px;border-radius:3px;}"
                "QComboBox::drop-down{border:none;width:18px;}"
                "QComboBox:hover{border-color:%4;}"
                "QComboBox QAbstractItemView{background:%1;color:%2;"
                "selection-background-color:%5;border:1px solid %3;"
                "font-size:11px;padding:2px;outline:none;}"
                "QComboBox QAbstractItemView::item{padding:4px 10px;min-height:22px;}"
                "QComboBox QLineEdit{background:%1;color:%2;border:none;"
                "selection-background-color:%5;font-size:10px;padding:0 4px;}")
            .arg(col::BG_BASE(), col::TEXT_PRIMARY(), col::BORDER_MED(), col::AMBER(), col::AMBER_DIM()));

    // When user picks an item from popup, clear search text and show the selected name
    connect(agent_selector_, QOverload<int>::of(&QComboBox::activated), agent_selector_, [this](int idx) {
        // Show full item text so truncation never occurs after selection
        agent_selector_->lineEdit()->setText(agent_selector_->itemText(idx));
        agent_selector_->lineEdit()->setCursorPosition(0);
    });

    hl->addWidget(agent_selector_);

    hl->addStretch();

    // Active model pill
    hdr_model_lbl_ = new QLabel(tr("No model"));
    hdr_model_lbl_->setStyleSheet(QString("color:%1;font-size:9px;background:%2;border:1px solid %3;"
                                          "border-radius:3px;padding:2px 8px;")
                                      .arg(col::TEXT_SECONDARY(), col::BG_BASE(), col::BORDER_MED()));
    hdr_model_lbl_->setToolTip(tr("Active LLM — configure in Settings > LLM Configuration"));
    hl->addWidget(hdr_model_lbl_);

    // Status chip
    hdr_status_lbl_ = new QLabel(tr("Ready"));
    hdr_status_lbl_->setFixedWidth(72);
    hdr_status_lbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::POSITIVE()));
    hl->addWidget(hdr_status_lbl_);

    auto* div2 = new QFrame;
    div2->setFrameShape(QFrame::VLine);
    div2->setFixedWidth(1);
    div2->setStyleSheet(QString("background:%1;").arg(col::BORDER_DIM()));
    hl->addWidget(div2);

    // Auto-route toggle
    route_toggle_ = new QPushButton(tr("AUTO-ROUTE"));
    route_toggle_->setCheckable(true);
    route_toggle_->setCursor(Qt::PointingHandCursor);
    route_toggle_->setFixedHeight(28);
    route_toggle_->setToolTip(tr("When ON, the system picks the best agent for each query."));
    route_toggle_->setStyleSheet(
        QString("QPushButton{background:transparent;color:%1;border:1px solid %2;"
                "padding:3px 10px;font-size:9px;font-weight:600;border-radius:3px;}"
                "QPushButton:checked{color:%3;border-color:%3;background:rgba(34,197,94,0.08);}"
                "QPushButton:hover{background:%4;}")
            .arg(col::TEXT_SECONDARY(), col::BORDER_MED(), col::POSITIVE(), col::BG_HOVER()));
    hl->addWidget(route_toggle_);

    // Run-as-task toggle — visible only when Agentic Mode is enabled.
    // When ON, send_message() dispatches via AgentService::start_task and the
    // query becomes a durable background task (visible in the AGENTIC tab).
    run_as_task_toggle_ = new QPushButton(tr("RUN AS TASK"));
    run_as_task_toggle_->setCheckable(true);
    run_as_task_toggle_->setCursor(Qt::PointingHandCursor);
    run_as_task_toggle_->setFixedHeight(28);
    run_as_task_toggle_->setToolTip(
        tr("When ON, this query runs as a durable background task with per-step progress."));
    run_as_task_toggle_->setStyleSheet(
        QString("QPushButton{background:transparent;color:%1;border:1px solid %2;"
                "padding:3px 10px;font-size:9px;font-weight:600;border-radius:3px;}"
                "QPushButton:checked{color:%3;border-color:%3;background:rgba(245,158,11,0.08);}"
                "QPushButton:hover{background:%4;}")
            .arg(col::TEXT_SECONDARY(), col::BORDER_MED(), col::AMBER(), col::BG_HOVER()));
    run_as_task_toggle_->setVisible(false); // gated by agentic_mode_enabled
    hl->addWidget(run_as_task_toggle_);

    // Clear button
    clear_btn_ = new QPushButton(tr("CLEAR"));
    clear_btn_->setCursor(Qt::PointingHandCursor);
    clear_btn_->setFixedHeight(28);
    clear_btn_->setStyleSheet(
        QString("QPushButton{background:transparent;color:%1;border:1px solid %2;"
                "padding:3px 10px;font-size:9px;font-weight:600;border-radius:3px;}"
                "QPushButton:hover{background:%3;color:%4;border-color:%4;}")
            .arg(col::TEXT_TERTIARY(), col::BORDER_DIM(), col::BG_HOVER(), col::TEXT_SECONDARY()));
    hl->addWidget(clear_btn_);
    root->addWidget(header);

    // ── Portfolio context bar ─────────────────────────────────────────────────
    auto* pbar = new QWidget(this);
    pbar->setFixedHeight(38);
    pbar->setStyleSheet(QString("background:%1;border-bottom:1px solid %2;").arg(col::BG_SURFACE(), col::BORDER_DIM()));
    auto* pl = new QHBoxLayout(pbar);
    pl->setContentsMargins(14, 0, 14, 0);
    pl->setSpacing(8);

    portfolio_caption_ = new QLabel(tr("PORTFOLIO:"));
    portfolio_caption_->setStyleSheet(
        QString("color:%1;font-size:9px;font-weight:600;letter-spacing:0.5px;").arg(col::TEXT_TERTIARY()));
    pl->addWidget(portfolio_caption_);

    portfolio_combo_ = new QComboBox;
    portfolio_combo_->addItem(tr("None"));
    portfolio_combo_->setFixedWidth(160);
    portfolio_combo_->setStyleSheet(QString("QComboBox{background:%1;color:%2;border:1px solid %3;padding:2px 8px;"
                                            "font-size:10px;border-radius:3px;}"
                                            "QComboBox::drop-down{border:none;}"
                                            "QComboBox QAbstractItemView{background:%1;color:%2;"
                                            "selection-background-color:%4;border:1px solid %3;}")
                                        .arg(col::BG_BASE(), col::TEXT_PRIMARY(), col::BORDER_DIM(), col::AMBER_DIM()));
    pl->addWidget(portfolio_combo_);

    auto qbtn = [&](const QString& label, const QString& clr) -> QPushButton* {
        auto* b = new QPushButton(label);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(24);
        b->setStyleSheet(QString("QPushButton{background:transparent;color:%1;border:1px solid %2;"
                                 "font-size:9px;font-weight:600;padding:2px 8px;border-radius:3px;}"
                                 "QPushButton:hover{background:rgba(255,255,255,0.05);border-color:%1;}")
                             .arg(clr, col::BORDER_DIM()));
        return b;
    };
    analyze_btn_ = qbtn(tr("ANALYZE"), col::CYAN);
    rebalance_btn_ = qbtn(tr("REBALANCE"), col::AMBER);
    risk_btn_ = qbtn(tr("RISK"), col::NEGATIVE);
    pl->addWidget(analyze_btn_);
    pl->addWidget(rebalance_btn_);
    pl->addWidget(risk_btn_);
    pl->addStretch();
    root->addWidget(pbar);

    // ── Messages scroll area ──────────────────────────────────────────────────
    scroll_area_ = new QScrollArea;
    scroll_area_->setWidgetResizable(true);
    scroll_area_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_area_->setStyleSheet(QString("QScrollArea{background:%1;border:none;}"
                                        "QScrollBar:vertical{background:%1;width:5px;border:none;}"
                                        "QScrollBar::handle:vertical{background:%2;border-radius:2px;min-height:20px;}"
                                        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}")
                                    .arg(col::BG_BASE(), col::BORDER_MED()));

    messages_container_ = new QWidget(this);
    messages_container_->setStyleSheet(QString("background:%1;").arg(col::BG_BASE()));
    messages_layout_ = new QVBoxLayout(messages_container_);
    messages_layout_->setContentsMargins(24, 20, 24, 20);
    messages_layout_->setSpacing(12);
    messages_layout_->addStretch();
    scroll_area_->setWidget(messages_container_);

    // Welcome panel (overlaid inside the scroll container indirectly via layout slot)
    welcome_panel_ = new QWidget(this);
    welcome_panel_->setStyleSheet(QString("background:%1;").arg(col::BG_BASE()));
    auto* wvl = new QVBoxLayout(welcome_panel_);
    wvl->setContentsMargins(48, 40, 48, 32);
    wvl->setSpacing(16);
    wvl->addStretch();

    welcome_title_ = new QLabel(tr("How can I help you?"));
    welcome_title_->setAlignment(Qt::AlignCenter);
    welcome_title_->setStyleSheet(
        QString("color:%1;font-size:22px;font-weight:700;background:transparent;").arg(col::TEXT_PRIMARY()));
    wvl->addWidget(welcome_title_);

    welcome_subtitle_ = new QLabel(tr("Ask about markets, portfolios, or any financial topic.\n"
                                      "Select an agent above, or use Auto-Route to let the system decide."));
    welcome_subtitle_->setAlignment(Qt::AlignCenter);
    welcome_subtitle_->setWordWrap(true);
    welcome_subtitle_->setStyleSheet(
        QString("color:%1;font-size:12px;background:transparent;").arg(col::TEXT_SECONDARY()));
    wvl->addWidget(welcome_subtitle_);

    // Suggestion chips
    auto* chip_row = new QHBoxLayout;
    chip_row->setSpacing(10);
    chip_row->addStretch();
    struct Chip {
        const char* label;
        const char* color;
        const char* query;
    };
    const Chip chips[] = {
        {"Markets", col::CYAN, "Show today's top market movers"},
        {"Portfolio", col::POSITIVE, "Analyze my portfolio performance"},
        {"Analytics", col::AMBER, "Calculate valuation for AAPL"},
        {"Risk", col::NEGATIVE, "Run risk analysis on my portfolio"},
    };
    for (const auto& c : chips) {
        auto* btn = new QPushButton(tr(c.label));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:1px solid %3;"
                                   "padding:6px 14px;font-size:10px;font-weight:600;border-radius:4px;}"
                                   "QPushButton:hover{background:%4;}")
                               .arg(col::BG_RAISED(), c.color, col::BORDER_MED(), col::BG_HOVER()));
        const QString q = tr(c.query);
        connect(btn, &QPushButton::clicked, this, [this, q]() {
            input_edit_->setPlainText(q);
            send_message();
        });
        chip_row->addWidget(btn);
    }
    chip_row->addStretch();
    wvl->addLayout(chip_row);
    wvl->addStretch();

    messages_layout_->insertWidget(0, welcome_panel_);

    // Typing indicator
    typing_indicator_ = new QWidget(this);
    typing_indicator_->setFixedHeight(28);
    typing_indicator_->setStyleSheet("background:transparent;");
    auto* til = new QHBoxLayout(typing_indicator_);
    til->setContentsMargins(4, 0, 0, 0);
    typing_dots_lbl_ = new QLabel(tr("Agent is thinking"));
    typing_dots_lbl_->setStyleSheet(
        QString("color:%1;font-size:11px;font-style:italic;background:transparent;").arg(col::TEXT_DIM()));
    til->addWidget(typing_dots_lbl_);
    til->addStretch();
    typing_indicator_->hide();
    messages_layout_->insertWidget(messages_layout_->count() - 1, typing_indicator_);

    root->addWidget(scroll_area_, 1);

    // ── Input bar ─────────────────────────────────────────────────────────────
    auto* ib = new QWidget(this);
    ib->setStyleSheet(QString("background:%1;border-top:1px solid %2;").arg(col::BG_RAISED(), col::BORDER_DIM()));
    auto* il = new QHBoxLayout(ib);
    il->setContentsMargins(14, 10, 14, 10);
    il->setSpacing(10);

    input_edit_ = new QTextEdit;
    input_edit_->setPlaceholderText(tr("Message agent... (Shift+Enter for new line, Enter to send)"));
    input_edit_->setFixedHeight(44);
    input_edit_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    input_edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    input_edit_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    input_edit_->setFrameShape(QFrame::NoFrame);
    input_edit_->setStyleSheet(QString("QTextEdit{background:%1;color:%2;border:1px solid %3;"
                                       "padding:8px 12px;font-size:13px;border-radius:4px;}"
                                       "QTextEdit:focus{border-color:%4;}")
                                   .arg(col::BG_BASE(), col::TEXT_PRIMARY(), col::BORDER_MED(), col::AMBER()));
    input_edit_->installEventFilter(this);
    input_edit_->setAccessibleName(tr("Message the selected agent"));
    input_edit_->setAccessibleDescription(tr("Enter sends, Shift+Enter inserts a new line."));
    // Grow height as user types (max ~120px / ~5 lines)
    connect(input_edit_->document(), &QTextDocument::contentsChanged, input_edit_, [this]() {
        int doc_h = static_cast<int>(input_edit_->document()->size().height());
        int new_h = qBound(44, doc_h + 18, 120);
        input_edit_->setFixedHeight(new_h);
    });
    il->addWidget(input_edit_, 1);

    send_btn_ = new QPushButton(tr("Send"));
    send_btn_->setFixedSize(76, 44);
    send_btn_->setCursor(Qt::PointingHandCursor);
    send_btn_->setAccessibleName(tr("Send message to agent"));
    send_btn_->setStyleSheet(
        QString("QPushButton{background:%1;color:%2;border:none;border-radius:6px;"
                "font-size:12px;font-weight:700;}"
                "QPushButton:hover:enabled{background:%3;}"
                "QPushButton:disabled{background:%4;color:%5;}")
            .arg(col::AMBER(), col::BG_BASE(), col::ORANGE(), col::BG_RAISED(), col::TEXT_TERTIARY()));
    il->addWidget(send_btn_);
    root->addWidget(ib);

    // Explicit tab order across the panel's interactive controls — creation
    // order otherwise walks header widgets before the composer.
    setTabOrder(agent_selector_, route_toggle_);
    setTabOrder(route_toggle_, run_as_task_toggle_);
    setTabOrder(run_as_task_toggle_, clear_btn_);
    setTabOrder(clear_btn_, portfolio_combo_);
    setTabOrder(portfolio_combo_, analyze_btn_);
    setTabOrder(analyze_btn_, rebalance_btn_);
    setTabOrder(rebalance_btn_, risk_btn_);
    setTabOrder(risk_btn_, input_edit_);
    setTabOrder(input_edit_, send_btn_);

    // ── Status bar ────────────────────────────────────────────────────────────
    status_label_ = new QLabel;
    status_label_->setTextFormat(Qt::PlainText);
    status_label_->setFixedHeight(18);
    status_label_->setStyleSheet(
        QString("background:%1;color:%2;font-size:9px;padding:0 14px;").arg(col::BG_SURFACE(), col::TEXT_TERTIARY()));
    root->addWidget(status_label_);
}

// ── Event filter (Enter to send) ─────────────────────────────────────────────

bool AgentChatPanel::eventFilter(QObject* obj, QEvent* event) {
    if (obj == e015_plan_body_ && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, [this]() {
            if (e015_plan_body_) {
                e015_plan_body_->document()->setTextWidth(qMax(200, e015_plan_body_->viewport()->width()));
                e015_plan_body_->setFixedHeight(qMax(48, static_cast<int>(e015_plan_body_->document()->size().height()) + 12));
            }
        });
    }
    if (obj == input_edit_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !(ke->modifiers() & Qt::ShiftModifier)) {
            ke->accept();
            send_message();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ── Connections ───────────────────────────────────────────────────────────────

void AgentChatPanel::setup_connections() {
    connect(send_btn_, &QPushButton::clicked, this, &AgentChatPanel::send_message);
    connect(clear_btn_, &QPushButton::clicked, this, &AgentChatPanel::clear_chat);
    connect(agent_selector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        update_e015_controls();
        sync_e015_polling();
    });

    connect(route_toggle_, &QPushButton::toggled, this, [this](bool on) {
        auto_routing_ = on;
        route_toggle_->setText(on ? tr("AUTO-ROUTE: ON") : tr("AUTO-ROUTE"));
        agent_selector_->setEnabled(!on);
    });

    connect(run_as_task_toggle_, &QPushButton::toggled, this, [this](bool on) {
        run_as_task_ = on;
        run_as_task_toggle_->setText(on ? tr("RUN AS TASK: ON") : tr("RUN AS TASK"));
    });

    // Visibility of the "RUN AS TASK" toggle follows the Agentic Mode setting.
    auto apply_agentic_visibility = [this]() {
        auto r = fincept::SettingsRepository::instance().get(QStringLiteral("agentic_mode_enabled"),
                                                             QStringLiteral("false"));
        const bool on = r.is_ok() && r.value() == QStringLiteral("true");
        run_as_task_toggle_->setVisible(on);
        if (!on) {
            run_as_task_toggle_->setChecked(false);
            run_as_task_ = false;
        }
    };
    apply_agentic_visibility();
    connect(&fincept::EventBus::instance(), &fincept::EventBus::eventPublished, this,
            [apply_agentic_visibility](const QString& event, const QVariantMap&) {
                if (event == QStringLiteral("settings.agentic_mode_changed"))
                    apply_agentic_visibility();
            });

    // Agent selector repopulation on discovery
    auto& svc = services::AgentService::instance();
    connect(&svc, &services::AgentService::agents_discovered, this,
            [this](QVector<services::AgentInfo> agents, QVector<services::AgentCategory>) {
                const QString prev_id = agent_selector_->currentData().toString();
                agent_selector_->blockSignals(true);
                agent_selector_->clear();
                agent_selector_->addItem(tr("Default (global LLM)"), QString{});
                ensure_e015_selector();
                for (const auto& a : agents)
                    if (a.id != kE015Agent)
                        agent_selector_->addItem(QString("[%1] %2").arg(a.category, a.name), a.id);
                // Restore previous selection
                int restore = 0;
                if (!prev_id.isEmpty()) {
                    for (int i = 1; i < agent_selector_->count(); ++i) {
                        if (agent_selector_->itemData(i).toString() == prev_id) {
                            restore = i;
                            break;
                        }
                    }
                }
                agent_selector_->blockSignals(false);
                agent_selector_->setCurrentIndex(restore);
                update_e015_controls();
                sync_e015_polling();
                // Keep completer model in sync after repopulation
                if (agent_selector_->completer())
                    agent_selector_->completer()->setModel(agent_selector_->model());
            });

    // LLM config changes
    connect(&ai_chat::LlmService::instance(), &ai_chat::LlmService::config_changed, this,
            &AgentChatPanel::update_llm_status, Qt::UniqueConnection);

    // Streaming signals
    connect(&svc, &services::AgentService::agent_stream_thinking, this,
            [this](const QString& req_id, const QString& status) {
                if (req_id != pending_request_id_)
                    return;
                status_label_->setText(status);
            });

    connect(&svc, &services::AgentService::agent_stream_token, this,
            [this](const QString& req_id, const QString& token) {
                if (req_id != pending_request_id_)
                    return;
                streaming_text_ += token;
                if (streaming_bubble_widget_) {
                    if (typing_indicator_->isVisible())
                        show_typing(false);
                    // Replace placeholder sentinel on first real token
                    if (streaming_bubble_widget_->toPlainText() == "...") {
                        streaming_bubble_widget_->setPlainText(token);
                    } else {
                        streaming_bubble_widget_->moveCursor(QTextCursor::End);
                        streaming_bubble_widget_->insertPlainText(token);
                    }
                    scroll_to_bottom();
                }
                status_label_->setText(tr("Streaming..."));
            });

    connect(&svc, &services::AgentService::agent_stream_done, this, [this](services::AgentExecutionResult r) {
        if (r.request_id != pending_request_id_)
            return;
        show_typing(false);
        set_executing(false);

        if (streaming_bubble_widget_) {
            if (!r.success) {
                streaming_bubble_widget_->setPlainText(tr("Error: %1").arg(r.error));
            } else {
                // Replace streamed plain-text tokens with fully rendered markdown HTML.
                const QString final_text = r.response.isEmpty() ? streaming_text_ : r.response;
                if (!final_text.trimmed().isEmpty()) {
                    streaming_bubble_widget_->setHtml(ui::MarkdownRenderer::render(final_text));
                    streaming_bubble_widget_->document()->setTextWidth(600);
                    const int h = static_cast<int>(streaming_bubble_widget_->document()->size().height());
                    streaming_bubble_widget_->setMinimumHeight(qMax(h + 8, 28));
                    streaming_bubble_widget_->setMaximumHeight(qMax(h + 8, 28));
                }
            }
            streaming_bubble_widget_->setReadOnly(true);
            streaming_bubble_widget_ = nullptr;
        } else if (r.success && !r.response.isEmpty()) {
            add_assistant_bubble(r.response);
        } else if (!r.success) {
            add_system_bubble(tr("Error: %1").arg(r.error));
        }

        streaming_text_.clear();
        if (r.success) {
            status_label_->setText(tr("Response received (%1ms)").arg(r.execution_time_ms));
            hdr_status_lbl_->setText(tr("Ready"));
            hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::POSITIVE()));
        } else {
            status_label_->setText(tr("Agent execution failed"));
            hdr_status_lbl_->setText(tr("Error"));
            hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::NEGATIVE()));
        }
        scroll_to_bottom();
    });

    // Failure that never produces an agent_result / agent_stream_done (Python
    // crash, spawn failure, bad config). Without this the send button stayed
    // disabled and `executing_` stuck true — the panel was dead until restart.
    // Mirrors the guard TeamsViewPanel / WorkflowsViewPanel already have.
    connect(&svc, &services::AgentService::error_occurred, this, [this](const QString&, const QString& msg) {
        if (!executing_ || e015_selected())
            return;
        show_typing(false);
        set_executing(false);
        if (streaming_bubble_widget_) {
            streaming_bubble_widget_->setPlainText(tr("Error: %1").arg(msg));
            streaming_bubble_widget_->setReadOnly(true);
            streaming_bubble_widget_ = nullptr;
        } else {
            add_system_bubble(tr("Error: %1").arg(msg));
        }
        streaming_text_.clear();
        status_label_->setText(tr("Agent execution failed"));
        hdr_status_lbl_->setText(tr("Error"));
        hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::NEGATIVE()));
    });

    // Routing result
    connect(&svc, &services::AgentService::routing_result, this, [this](services::RoutingResult r) {
        if (r.request_id != pending_request_id_)
            return;
        if (r.success) {
            add_system_bubble(tr("Routed to: %1 (intent: %2, confidence: %3%)")
                                  .arg(r.agent_id, r.intent)
                                  .arg(static_cast<int>(r.confidence * 100)));
            pending_request_id_ = services::AgentService::instance().run_agent_streaming(last_query_, r.config);
        } else {
            add_system_bubble(tr("Auto-routing failed — using default agent."));
            pending_request_id_ = services::AgentService::instance().run_agent_streaming(last_query_, {});
        }
    });

    // Portfolio quick actions. The "None" entry has no data role, so an empty
    // currentData() means "no portfolio selected" (survives translation).
    connect(analyze_btn_, &QPushButton::clicked, this, [this]() {
        const QString pf = portfolio_combo_->currentText();
        if (portfolio_combo_->currentData().toString().isEmpty())
            return;
        input_edit_->setPlainText(tr("Analyze my portfolio '%1' — give key metrics and recommendations.").arg(pf));
        send_message();
    });
    connect(rebalance_btn_, &QPushButton::clicked, this, [this]() {
        const QString pf = portfolio_combo_->currentText();
        if (portfolio_combo_->currentData().toString().isEmpty())
            return;
        input_edit_->setPlainText(tr("Suggest rebalancing for portfolio '%1' to optimize risk-return.").arg(pf));
        send_message();
    });
    connect(risk_btn_, &QPushButton::clicked, this, [this]() {
        const QString pf = portfolio_combo_->currentText();
        if (portfolio_combo_->currentData().toString().isEmpty())
            return;
        input_edit_->setPlainText(tr("Perform risk analysis on portfolio '%1' — VaR, drawdown, stress test.").arg(pf));
        send_message();
    });
}

// ── showEvent ─────────────────────────────────────────────────────────────────

void AgentChatPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!e015_selected()) {
        services::AgentService::instance().discover_agents();
        refresh_portfolios();
    }
    update_llm_status();
    update_e015_controls();
    sync_e015_polling();
}

void AgentChatPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    stop_e015_requests();
}

// ── LLM status display ────────────────────────────────────────────────────────

void AgentChatPanel::update_llm_status() {
    auto& llm = ai_chat::LlmService::instance();
    if (llm.is_configured()) {
        const QString label = QString("%1 / %2").arg(llm.active_provider(), llm.active_model());
        hdr_model_lbl_->setText(label);
        hdr_model_lbl_->setStyleSheet(QString("color:%1;font-size:9px;background:%2;border:1px solid %3;"
                                              "border-radius:3px;padding:2px 8px;")
                                          .arg(col::TEXT_SECONDARY(), col::BG_BASE(), col::BORDER_MED()));
        hdr_status_lbl_->setText(tr("Ready"));
        hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::POSITIVE()));
    } else {
        hdr_model_lbl_->setText(tr("No LLM configured"));
        hdr_model_lbl_->setStyleSheet(QString("color:%1;font-size:9px;background:%2;border:1px solid %3;"
                                              "border-radius:3px;padding:2px 8px;")
                                          .arg(col::NEGATIVE(), col::BG_BASE(), col::NEGATIVE()));
        hdr_status_lbl_->setText(tr("Unconfigured"));
        hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::NEGATIVE()));
        status_label_->setText(tr("No LLM provider configured — go to Settings > LLM Configuration"));
    }
    if (e015_selected()) {
        if (!e015_configured()) {
            stop_e015_requests();
            show_e015_status(tr("需配置 gpt-6-astra、本地地址 http://127.0.0.1:18769/v1 和认证令牌。"), true);
        } else {
            sync_e015_polling();
        }
    }
}

// ── E015 authenticated local bridge ──────────────────────────────────────────

bool AgentChatPanel::e015_selected() const {
    return agent_selector_->currentData().toString() == kE015Agent;
}

bool AgentChatPanel::e015_active() const {
    return e015_selected() && isVisible() && qApp->applicationState() == Qt::ApplicationActive;
}

bool AgentChatPanel::e015_configured() const {
    const auto& llm = ai_chat::LlmService::instance();
    const QString token = llm.active_api_key();
    return llm.active_model() == QStringLiteral("gpt-6-astra") &&
           llm.active_base_url() == QStringLiteral("http://127.0.0.1:18769/v1") &&
           !token.trimmed().isEmpty() && !token.contains('\r') && !token.contains('\n');
}

void AgentChatPanel::ensure_e015_selector() {
    if (agent_selector_->findData(kE015Agent) < 0)
        agent_selector_->addItem(tr("E015 策略AI（实时计划）"), kE015Agent);
}

void AgentChatPanel::update_e015_controls() {
    const bool selected = e015_selected();
    if (selected) {
        route_toggle_->setChecked(false);
        run_as_task_toggle_->setChecked(false);
        // Ignore any legacy agent completion after changing to this local brain.
        if (!pending_request_id_.isEmpty()) {
            pending_request_id_.clear();
            show_typing(false);
            set_executing(false);
            streaming_bubble_widget_ = nullptr;
            streaming_text_.clear();
        }
    }
    route_toggle_->setEnabled(!selected);
    run_as_task_toggle_->setEnabled(!selected);
    portfolio_combo_->setEnabled(!selected);
    analyze_btn_->setEnabled(!selected);
    rebalance_btn_->setEnabled(!selected);
    risk_btn_->setEnabled(!selected);
    portfolio_caption_->setText(selected ? tr("原始 E015 账本与策略") : tr("PORTFOLIO:"));
    portfolio_caption_->setToolTip(selected ? tr("读取原始 E015 持仓、资金与策略；不使用 Fincept 的重复组合。") : QString{});
    status_label_->setFixedHeight(selected ? 42 : 18);
    status_label_->setWordWrap(selected);
    status_label_->setStyleSheet(QString("background:%1;color:%2;font-size:%3px;padding:0 14px;")
                                     .arg(col::BG_SURFACE(), col::TEXT_TERTIARY()).arg(selected ? 14 : 9));
    if (e015_plan_panel_)
        e015_plan_panel_->setVisible(selected);
}

void AgentChatPanel::sync_e015_polling() {
    if (!e015_poll_timer_)
        return;
    if (!e015_active()) {
        stop_e015_requests();
        return;
    }
    if (!e015_configured()) {
        stop_e015_requests();
        show_e015_status(tr("需配置 gpt-6-astra、本地地址 http://127.0.0.1:18769/v1 和认证令牌。"), true);
        return;
    }
    e015_poll_timer_->start();
    check_e015_version();
}

void AgentChatPanel::stop_e015_requests() {
    e015_version_unverified_ = true;
    if (e015_poll_timer_)
        e015_poll_timer_->stop();
    if (e015_plan_reply_)
        e015_attempted_version_.clear(); // An interrupted request was never a completed attempt.
    for (auto* slot : {&e015_version_reply_, &e015_plan_reply_}) {
        if (*slot) {
            auto* reply = slot->data();
            *slot = nullptr;
            reply->abort();
        }
    }
    if (!e015_manual_prompt_.isEmpty())
        set_executing(false); // The queued prompt resumes on activation; the draft stays intact.
}

QNetworkReply* AgentChatPanel::start_e015_request(const QString& path, const QByteArray& body) {
    if (!e015_active() || !e015_configured())
        return nullptr;
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:18769") + path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + ai_chat::LlmService::instance().active_api_key().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(125000);
    auto* reply = body.isEmpty() ? e015_network_->get(request) : e015_network_->post(request, body);
    reply->setReadBufferSize(kE015ResponseLimit + 1);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("e015_timeout", true);
        reply->abort();
    });
    timeout->start(125000); // Absolute deadline also bounds continuously streaming replies.
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply]() {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > kE015ResponseLimit) {
            reply->setProperty("e015_oversize", true);
            reply->abort();
        }
    });
    const auto bytes = std::make_shared<QByteArray>();
    const auto collect = [reply, bytes]() {
        const auto chunk = reply->read(kE015ResponseLimit - bytes->size() + 1);
        if (bytes->size() + chunk.size() > kE015ResponseLimit) {
            reply->setProperty("e015_oversize", true);
            reply->abort();
        } else {
            bytes->append(chunk);
            reply->setProperty("e015_body", *bytes);
        }
    };
    connect(reply, &QNetworkReply::readyRead, reply, collect);
    connect(reply, &QNetworkReply::finished, reply, collect);
    return reply;
}

// Errors are displayed as plain text, capped, and scrubbed of the active token.
static QString e015_reply_error(QNetworkReply* reply, const QJsonObject& object) {
    QString message;
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->property("e015_oversize").toBool())
        message = QStringLiteral("响应超过 2MB 限制");
    else if (reply->property("e015_timeout").toBool())
        message = QStringLiteral("请求超时");
    else if (http >= 300 && http < 400)
        message = QStringLiteral("本地桥接重定向已拒绝");
    else if (reply->error() != QNetworkReply::NoError || http != 200)
        message = QStringLiteral("HTTP %1：%2").arg(http).arg(object.value("error").toObject().value("message").toString(
            QStringLiteral("本地桥接请求失败")));
    const QString token = ai_chat::LlmService::instance().active_api_key();
    if (!token.isEmpty())
        message.replace(token, QStringLiteral("[已隐藏]"));
    return message.left(400);
}

void AgentChatPanel::check_e015_version() {
    if (!e015_active() || e015_version_reply_)
        return;
    if (!e015_configured()) {
        stop_e015_requests();
        show_e015_status(tr("E015 本地桥接配置或认证令牌无效。"), true);
        return;
    }
    auto* reply = start_e015_request(QStringLiteral("/e015/version"));
    if (!reply)
        return;
    e015_version_reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (e015_version_reply_ != reply)
            return;
        e015_version_reply_ = nullptr;
        if (!e015_active())
            return;
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(reply->property("e015_body").toByteArray(), &parse_error);
        const auto object = doc.object();
        const QString error = e015_reply_error(reply, object);
        if (!error.isEmpty() || parse_error.error != QJsonParseError::NoError || !doc.isObject() ||
            object.value("version").toString().isEmpty() || !object.value("no_orders_sent").toBool() ||
            !object.value("no_account_writes").toBool()) {
            e015_version_unverified_ = true;
            show_e015_status(error.isEmpty() ? tr("E015 版本响应无效，当前计划待核验。") : error, true);
            if (!e015_plan_reply_)
                set_executing(false);
            return;
        }
        const QString version = object.value("version").toString();
        const bool changed = version != e015_version_;
        const bool restore_verified = e015_version_unverified_ &&
                                      e015_last_plan_.value("version").toString() == version;
        e015_version_unverified_ = false;
        e015_version_ = version;
        e015_generated_at_ = object.value("generated_at").toString();
        if (changed) {
            show_e015_status(tr("数据变化，正在更新"), true);
            e015_refresh_deferred_ = true;
        }
        if (!e015_plan_reply_) {
            if (!e015_manual_prompt_.isEmpty())
                request_e015_plan(false);
            else if (e015_attempted_version_ != e015_version_)
                request_e015_plan(true);
            else if (restore_verified)
                display_e015_plan(e015_last_plan_);
        }
    });
}

void AgentChatPanel::request_e015_plan(bool automatic) {
    if (!e015_active() || !e015_configured() || e015_plan_reply_ || e015_version_.isEmpty())
        return;
    const QString version = e015_version_;
    const QString prompt = automatic ? tr("根据最新已登记持仓和资金，给出当前可执行与后续分步计划；保持原批次和追加计划顺序。")
                                     : e015_manual_prompt_;
    const QJsonObject body{{"prompt", prompt}, {"expected_version", version}, {"automatic", automatic}};
    auto* reply = start_e015_request(QStringLiteral("/e015/plan"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!reply)
        return;
    e015_plan_reply_ = reply;
    e015_attempted_version_ = version;
    e015_refresh_deferred_ = false;
    if (!automatic)
        set_executing(true);
    show_e015_status(automatic ? tr("数据变化，正在更新") : tr("正在生成 E015 计划"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, automatic, version]() {
        reply->deleteLater();
        if (e015_plan_reply_ != reply)
            return;
        e015_plan_reply_ = nullptr;
        if (!automatic) {
            e015_manual_prompt_.clear();
            set_executing(false);
        }
        if (!e015_active())
            return;
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(reply->property("e015_body").toByteArray(), &parse_error);
        const auto object = doc.object();
        const QString error = e015_reply_error(reply, object);
        const QString status = object.value("status").toString();
        const bool stale = status == QStringLiteral("stale") || version != e015_version_ ||
                           object.value("version").toString() != e015_version_ ||
                           (!object.value("latest_version").toString().isEmpty() &&
                            object.value("latest_version").toString() != e015_version_);
        if (!error.isEmpty() || parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            show_e015_status(error.isEmpty() ? tr("E015 计划响应无效。") : error, true);
        } else if (stale) {
            show_e015_status(tr("计划版本已过期；数据变化，正在更新"), true);
            check_e015_version(); // Recheck once; unchanged failed inputs never trigger a retry loop.
        } else if (status == QStringLiteral("current") || status == QStringLiteral("ai_unavailable")) {
            display_e015_plan(object);
        } else {
            show_e015_status(tr("E015 计划状态无效。"), true);
        }
        if (e015_refresh_deferred_ && !e015_version_reply_ && e015_attempted_version_ != e015_version_)
            request_e015_plan(true);
    });
}

void AgentChatPanel::show_e015_status(const QString& text, bool invalidate) {
    if (!e015_plan_panel_) {
        auto* panel = new QFrame(messages_container_);
        panel->setStyleSheet(bubble_style("assistant"));
        auto* layout = new QVBoxLayout(panel);
        auto* label = new QLabel;
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setStyleSheet(QString("color:%1;font-size:14px;background:transparent;").arg(col::TEXT_SECONDARY()));
        auto* body = new QTextEdit;
        body->installEventFilter(this);
        body->setReadOnly(true);
        body->setFrameShape(QFrame::NoFrame);
        body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        body->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        body->setStyleSheet(QString("background:transparent;color:%1;font-size:16px;border:none;").arg(col::TEXT_PRIMARY()));
        auto resize_body = [body]() {
            body->document()->setTextWidth(qMax(200, body->viewport()->width()));
            body->setFixedHeight(qMax(48, static_cast<int>(body->document()->size().height()) + 12));
        };
        connect(body->document(), &QTextDocument::contentsChanged, body, resize_body);
        layout->addWidget(label);
        auto* shortcuts = new QHBoxLayout;
        shortcuts->setSpacing(8);
        const auto add_shortcut = [this, shortcuts](const QString& title, const QString& prompt) {
            auto* button = new QPushButton(title);
            button->setMinimumHeight(32);
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(tr("本地只读查询，使用原始 E015 账本与策略。"));
            button->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:1px solid %3;"
                                         "font-size:14px;padding:4px 12px;border-radius:3px;}"
                                         "QPushButton:hover{border-color:%2;}")
                                      .arg(col::BG_BASE(), col::CYAN(), col::BORDER_MED()));
            connect(button, &QPushButton::clicked, this, [this, prompt]() {
                if (!e015_active() || !e015_configured() || executing_)
                    return;
                const QString draft = input_edit_->toPlainText();
                input_edit_->setPlainText(prompt);
                send_message();
                input_edit_->setPlainText(draft);
            });
            shortcuts->addWidget(button);
        };
        add_shortcut(tr("持仓"), QStringLiteral("查看全部持仓"));
        add_shortcut(tr("规则"), QStringLiteral("查看策略规则"));
        add_shortcut(tr("批次投后"), QStringLiteral("查看批次投后金额"));
        add_shortcut(tr("交易记录"), QStringLiteral("查看交易记录"));
        shortcuts->addStretch();
        layout->addLayout(shortcuts);
        layout->addWidget(body);
        messages_layout_->insertWidget(messages_layout_->count() - 1, panel);
        e015_plan_panel_ = panel;
        e015_plan_status_ = label;
        e015_plan_body_ = body;
    }
    show_welcome(false);
    e015_plan_panel_->setVisible(e015_selected());
    if (invalidate)
        e015_plan_body_->setPlainText(tr("当前计划待更新，请勿执行旧版本。"));
    e015_plan_status_->setText(tr("E015 策略AI · %1\n源版本：%2 · 数据时间：%3\n原始 E015 账本与策略；仅生成计划，不下单；真实成交须登记")
                                  .arg(text, e015_version_.isEmpty() ? tr("待获取") : e015_version_, e015_generated_at_));
    status_label_->setText(text);
    hdr_status_lbl_->setText(tr("E015"));
}

void AgentChatPanel::display_e015_plan(const QJsonObject& plan) {
    QString status = plan.value("status").toString() == QStringLiteral("ai_unavailable")
                         ? tr("确定性计划已更新；AI 暂不可用") : tr("当前计划已更新");
    QStringList warnings;
    for (const auto& warning : plan.value("warnings").toArray())
        if (warning.isString())
            warnings.append(warning.toString());
    if (!warnings.isEmpty())
        status += QStringLiteral("\n") + warnings.join(QStringLiteral("\n"));
    e015_generated_at_ = plan.value("generated_at").toString();
    show_e015_status(status);
    const QString engine = plan.value("engine_text").toString();
    if (engine.trimmed().isEmpty()) {
        show_e015_status(tr("缺少确定性引擎计划，当前计划不可用。"), true);
        return;
    }
    e015_last_plan_ = plan;
    const QString assistant = plan.value("assistant_text").toString();
    e015_plan_body_->setHtml(e015_markdown(tr("## E015 确定性执行计划\n\n") + engine +
                                         (assistant.isEmpty() ? QString{} : tr("\n\n## AI 解释\n\n") + assistant)));
    scroll_to_bottom();
}

// ── send_message ─────────────────────────────────────────────────────────────

void AgentChatPanel::send_message() {
    const QString text = input_edit_->toPlainText().trimmed();
    if (text.isEmpty() || executing_)
        return;

    if (e015_selected()) {
        if (!e015_active() || !e015_configured()) {
            show_e015_status(tr("E015 仅在窗口激活时使用已认证的本地桥接；请检查模型、地址和令牌。"), true);
            return;
        }
        // An explicit follow-up supersedes an automatic refresh, preserving the draft.
        if (e015_plan_reply_) {
            auto* reply = e015_plan_reply_.data();
            e015_plan_reply_ = nullptr;
            reply->abort();
        }
        e015_manual_prompt_ = text;
        add_user_bubble(text);
        input_edit_->clear();
        set_executing(true);
        if (e015_version_.isEmpty())
            check_e015_version();
        else
            request_e015_plan(false);
        return;
    }

    // Guard: require LLM config
    auto& llm = ai_chat::LlmService::instance();
    if (!llm.is_configured()) {
        add_system_bubble(tr("No LLM provider configured. Go to Settings > LLM Configuration to set one up."));
        return;
    }

    set_executing(true);
    show_welcome(false);

    // Show the raw user text in the bubble (without the injected context blob)
    add_user_bubble(text);
    input_edit_->clear();
    input_edit_->setFixedHeight(44);

    // Build enriched query: prepend portfolio context if one is selected
    const QString pf_ctx = build_portfolio_context();
    last_query_ = pf_ctx.isEmpty() ? text : QString("%1\n\nUser question: %2").arg(pf_ctx, text);

    // Show which portfolio is active in the status bar
    if (!pf_ctx.isEmpty()) {
        const QString pf_name = portfolio_combo_->currentText();
        status_label_->setText(tr("Portfolio context: %1").arg(pf_name));
    }

    // Create streaming bubble — seed with "..." so height bootstraps correctly
    auto* te = add_streaming_bubble();
    te->setPlainText("...");
    streaming_bubble_widget_ = te;
    show_typing(true);
    scroll_to_bottom();

    if (run_as_task_) {
        // Agentic Mode: dispatch the query as a durable background task. The
        // chat bubble closes immediately with a confirmation; live progress is
        // shown in the AGENTIC tab (and on the task:event:* DataHub topic).
        const QString agent_id = agent_selector_->currentData().toString();
        QJsonObject config;
        if (!agent_id.isEmpty())
            config["agent_id"] = agent_id;
        pending_request_id_ = services::AgentService::instance().start_task(last_query_, config);
        if (streaming_bubble_widget_) {
            streaming_bubble_widget_->setPlainText(tr("Task started. Open the AGENTIC tab to watch progress."));
            streaming_bubble_widget_->setReadOnly(true);
            streaming_bubble_widget_ = nullptr;
        }
        show_typing(false);
        set_executing(false);
    } else if (auto_routing_) {
        pending_request_id_ = services::AgentService::instance().route_query(last_query_);
    } else {
        const QString agent_id = agent_selector_->currentData().toString();
        QJsonObject config;
        if (!agent_id.isEmpty())
            config["agent_id"] = agent_id;
        pending_request_id_ = services::AgentService::instance().run_agent_streaming(last_query_, config);
    }
}

// ── Bubble builders ───────────────────────────────────────────────────────────

// ── resizeEvent — keep bubbles at most 72% of panel width ────────────────────

void AgentChatPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!messages_container_)
        return;
    if (e015_plan_body_) {
        QTimer::singleShot(0, this, [this]() {
            if (!e015_plan_body_)
                return;
            e015_plan_body_->document()->setTextWidth(qMax(200, e015_plan_body_->viewport()->width()));
            e015_plan_body_->setFixedHeight(qMax(48, static_cast<int>(e015_plan_body_->document()->size().height()) + 12));
        });
    }
    const int max_user = static_cast<int>(width() * 0.72);
    const int max_ai = static_cast<int>(width() * 0.82);
    // Update all col_w widgets — they are direct children of row widgets in the layout
    for (int i = 0; i < messages_layout_->count(); ++i) {
        auto* item = messages_layout_->itemAt(i);
        if (!item->widget())
            continue;
        auto* row = item->widget();
        // Each row has one QHBoxLayout with a col_w inside
        auto* rl = qobject_cast<QHBoxLayout*>(row->layout());
        if (!rl)
            continue;
        for (int j = 0; j < rl->count(); ++j) {
            auto* wi = rl->itemAt(j);
            if (!wi->widget())
                continue;
            auto* col_w = wi->widget();
            if (col_w->objectName() == "bubble_user")
                col_w->setMaximumWidth(max_user);
            else if (col_w->objectName() == "bubble_ai")
                col_w->setMaximumWidth(max_ai);
        }
    }
}

// ── Portfolio context builder ─────────────────────────────────────────────────

QString AgentChatPanel::build_portfolio_context() const {
    const QString pf_name = portfolio_combo_->currentText();
    // The first combo item ("None") carries no data role — treat any empty-data
    // selection as "no portfolio" so the check survives translation.
    if (portfolio_combo_->currentData().toString().isEmpty() || pf_name.isEmpty())
        return {};

    const QString pf_id = portfolio_combo_->currentData().toString();
    QString ctx = QString("[Portfolio context: %1]\n").arg(pf_name);

    // Fetch assets
    const auto assets_result = PortfolioRepository::instance().get_assets(pf_id);
    if (assets_result.is_ok() && !assets_result.value().isEmpty()) {
        ctx += "Holdings:\n";
        for (const auto& a : assets_result.value()) {
            ctx += QString("  %1: qty=%2, avg_cost=%3\n")
                       .arg(a.symbol)
                       .arg(a.quantity, 0, 'f', 4)
                       .arg(a.avg_buy_price, 0, 'f', 2);
        }
    }
    return ctx;
}

void AgentChatPanel::refresh_portfolios() {
    portfolio_combo_->blockSignals(true);
    const QString prev = portfolio_combo_->currentText();
    portfolio_combo_->clear();
    portfolio_combo_->addItem(tr("None"));
    const auto result = PortfolioRepository::instance().list_portfolios();
    if (result.is_ok()) {
        for (const auto& p : result.value())
            portfolio_combo_->addItem(p.name, p.id);
    }
    const int idx = portfolio_combo_->findText(prev);
    portfolio_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
    portfolio_combo_->blockSignals(false);
}

// ── Bubble builders ───────────────────────────────────────────────────────────

void AgentChatPanel::add_user_bubble(const QString& text) {
    const QString ts = QDateTime::currentDateTime().toString("HH:mm");
    const QString role = "user";

    auto* row = new QWidget(this);
    row->setStyleSheet("background:transparent;");
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    rl->addStretch();

    auto* col_w = new QWidget(this);
    col_w->setObjectName("bubble_user");
    col_w->setStyleSheet("background:transparent;");
    col_w->setMaximumWidth(static_cast<int>(width() * 0.72));
    auto* cvl = new QVBoxLayout(col_w);
    cvl->setContentsMargins(0, 0, 0, 0);
    cvl->setSpacing(4);

    auto* role_lbl = new QLabel(role_label(role));
    role_lbl->setAlignment(Qt::AlignRight);
    role_lbl->setStyleSheet(
        QString("color:%1;font-size:9px;font-weight:600;background:transparent;").arg(role_color(role)));
    cvl->addWidget(role_lbl);

    auto* bubble = new QFrame;
    bubble->setStyleSheet(bubble_style(role));
    auto* bvl = new QVBoxLayout(bubble);
    bvl->setContentsMargins(0, 0, 0, 0);
    auto* body = new QLabel(text);
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setTextFormat(Qt::PlainText);
    body->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(body_color(role)));
    bvl->addWidget(body);
    cvl->addWidget(bubble);

    auto* ts_lbl = new QLabel(ts);
    ts_lbl->setAlignment(Qt::AlignRight);
    ts_lbl->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;").arg(col::TEXT_DIM()));
    cvl->addWidget(ts_lbl);

    rl->addWidget(col_w);
    messages_layout_->insertWidget(messages_layout_->count() - 1, row);
    scroll_to_bottom();
}

void AgentChatPanel::add_assistant_bubble(const QString& text, const QString& agent_name) {
    const QString ts = QDateTime::currentDateTime().toString("HH:mm");
    const QString role = "assistant";

    auto* row = new QWidget(this);
    row->setStyleSheet("background:transparent;");
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);

    auto* col_w = new QWidget(this);
    col_w->setObjectName("bubble_ai");
    col_w->setStyleSheet("background:transparent;");
    col_w->setMaximumWidth(static_cast<int>(width() * 0.82));
    auto* cvl = new QVBoxLayout(col_w);
    cvl->setContentsMargins(0, 0, 0, 0);
    cvl->setSpacing(4);

    auto* hdr_row = new QHBoxLayout;
    auto* role_lbl = new QLabel(agent_name.isEmpty() ? tr("Agent") : agent_name);
    role_lbl->setStyleSheet(
        QString("color:%1;font-size:9px;font-weight:600;background:transparent;").arg(role_color(role)));
    hdr_row->addWidget(role_lbl);
    hdr_row->addStretch();
    cvl->addLayout(hdr_row);

    auto* bubble = new QFrame;
    bubble->setStyleSheet(bubble_style(role));
    auto* bvl = new QVBoxLayout(bubble);
    bvl->setContentsMargins(0, 0, 0, 0);

    // Use QTextEdit so markdown tables/bold/lists render correctly.
    // QLabel with Qt::MarkdownText does not support tables.
    auto* body = new QTextEdit;
    body->setReadOnly(true);
    body->setFrameShape(QFrame::NoFrame);
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    body->document()->setDocumentMargin(4);
    body->setStyleSheet(
        QString("QTextEdit{background:transparent;color:%1;border:none;font-size:12px;}").arg(body_color(role)));
    body->setHtml(ui::MarkdownRenderer::render(text));
    // Size to content using same fixed-width approach as streaming bubble
    body->document()->setTextWidth(600);
    const int doc_h = static_cast<int>(body->document()->size().height());
    body->setMinimumHeight(qMax(doc_h + 8, 28));
    body->setMaximumHeight(qMax(doc_h + 8, 28));

    bvl->addWidget(body);
    cvl->addWidget(bubble);

    auto* ts_lbl = new QLabel(ts);
    ts_lbl->setAlignment(Qt::AlignLeft);
    ts_lbl->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;").arg(col::TEXT_DIM()));
    cvl->addWidget(ts_lbl);

    rl->addWidget(col_w);
    rl->addStretch();
    messages_layout_->insertWidget(messages_layout_->count() - 1, row);
    scroll_to_bottom();
}

void AgentChatPanel::add_system_bubble(const QString& text) {
    const QString role = "system";

    auto* row = new QWidget(this);
    row->setStyleSheet("background:transparent;");
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addStretch();

    auto* bubble = new QFrame;
    bubble->setStyleSheet(bubble_style(role));
    bubble->setMaximumWidth(static_cast<int>(width() * 0.60));
    auto* bvl = new QHBoxLayout(bubble);
    bvl->setContentsMargins(0, 0, 0, 0);
    auto* body = new QLabel(text);
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;").arg(body_color(role)));
    bvl->addWidget(body);

    rl->addWidget(bubble);
    rl->addStretch();
    messages_layout_->insertWidget(messages_layout_->count() - 1, row);
    scroll_to_bottom();
}

QTextEdit* AgentChatPanel::add_streaming_bubble(const QString& agent_name) {
    auto* row = new QWidget(this);
    row->setStyleSheet("background:transparent;");
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);

    auto* col_w = new QWidget(this);
    col_w->setObjectName("bubble_ai");
    col_w->setStyleSheet("background:transparent;");
    col_w->setMaximumWidth(static_cast<int>(width() * 0.82));
    auto* cvl = new QVBoxLayout(col_w);
    cvl->setContentsMargins(0, 0, 0, 0);
    cvl->setSpacing(4);

    auto* role_lbl = new QLabel(agent_name.isEmpty() ? tr("Agent") : agent_name);
    role_lbl->setStyleSheet(QString("color:%1;font-size:9px;font-weight:600;background:transparent;").arg(col::CYAN()));
    cvl->addWidget(role_lbl);

    auto* bubble = new QFrame;
    bubble->setStyleSheet(bubble_style("assistant"));
    auto* bvl = new QVBoxLayout(bubble);
    bvl->setContentsMargins(0, 0, 0, 0);

    auto* body = new QTextEdit;
    // Read-only from the start — programmatic setPlainText/insertPlainText still
    // work, and the user can no longer type into the agent's answer mid-stream.
    body->setReadOnly(true);
    body->setFrameShape(QFrame::NoFrame);
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    body->document()->setDocumentMargin(4);
    body->setMinimumHeight(28);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    body->setStyleSheet(
        QString("QTextEdit{background:transparent;color:%1;border:none;font-size:12px;}").arg(col::TEXT_PRIMARY()));
    // Reliable auto-grow: recompute at a fixed known width (600px matches col_w maxWidth 680
    // minus bubble padding). No dependency on viewport()->width() which can be 0 pre-layout.
    auto recompute_height = [body]() {
        body->document()->setTextWidth(600);
        const int doc_h = static_cast<int>(body->document()->size().height());
        body->setMinimumHeight(qMax(doc_h + 8, 28));
        body->setMaximumHeight(qMax(doc_h + 8, 28));
    };
    connect(body->document(), &QTextDocument::contentsChanged, body, recompute_height);
    bvl->addWidget(body);
    cvl->addWidget(bubble);

    rl->addWidget(col_w);
    rl->addStretch();
    messages_layout_->insertWidget(messages_layout_->count() - 1, row);
    return body;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void AgentChatPanel::scroll_to_bottom() {
    QTimer::singleShot(50, this, [this]() {
        scroll_area_->verticalScrollBar()->setValue(scroll_area_->verticalScrollBar()->maximum());
    });
}

void AgentChatPanel::set_executing(bool on) {
    executing_ = on;
    send_btn_->setEnabled(!on);
    send_btn_->setText(on ? tr("...") : tr("Send"));
    if (on) {
        hdr_status_lbl_->setText(tr("Streaming"));
        hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::AMBER()));
        status_label_->setText(tr("Processing..."));
    }
    if (!on) {
        pending_request_id_.clear();
    }
}

void AgentChatPanel::show_welcome(bool on) {
    if (welcome_panel_)
        welcome_panel_->setVisible(on);
}

void AgentChatPanel::show_typing(bool on) {
    if (!typing_indicator_)
        return;
    if (on) {
        // Keep the indicator immediately above the trailing stretch, i.e. below
        // the newest bubble. Bubbles are inserted at count()-1 too, so without
        // this re-anchor the indicator stays wherever it was first inserted and
        // "Agent is thinking" renders at the TOP of the transcript.
        const int last = messages_layout_->count() - 1; // the stretch
        if (messages_layout_->indexOf(typing_indicator_) != last - 1) {
            messages_layout_->removeWidget(typing_indicator_);
            messages_layout_->insertWidget(messages_layout_->count() - 1, typing_indicator_);
        }
        typing_step_ = 0;
        typing_dots_lbl_->setText(tr("Agent is thinking"));
        typing_indicator_->show();
        typing_timer_->start();
    } else {
        typing_timer_->stop();
        typing_indicator_->hide();
    }
}

void AgentChatPanel::clear_chat() {
    stop_e015_requests();
    e015_manual_prompt_.clear();
    e015_attempted_version_.clear();
    e015_last_plan_ = {};
    e015_version_unverified_ = false;
    e015_plan_panel_ = nullptr;
    e015_plan_status_ = nullptr;
    e015_plan_body_ = nullptr;
    // Null streaming refs before deleting widgets
    streaming_bubble_widget_ = nullptr;
    streaming_text_.clear();
    show_typing(false);
    set_executing(false);

    // Remove every message row, keeping the two fixtures and the trailing
    // stretch. The old version assumed the typing indicator sat at count-2 and
    // blindly took index 1 — but new bubbles are inserted *after* the typing
    // indicator, so index 1 WAS the typing indicator. Clearing therefore
    // deleteLater()'d it (leaving `typing_indicator_` dangling — the next send
    // called show()/setText() on freed memory) and always left the last message
    // row behind. Identify the fixtures by pointer instead of by index.
    for (int i = messages_layout_->count() - 1; i >= 0; --i) {
        QLayoutItem* item = messages_layout_->itemAt(i);
        QWidget* w = item ? item->widget() : nullptr;
        if (!w || w == welcome_panel_ || w == typing_indicator_)
            continue; // stretch (no widget) + the two permanent fixtures
        QLayoutItem* taken = messages_layout_->takeAt(i);
        w->deleteLater();
        delete taken;
    }

    show_welcome(true);
    hdr_status_lbl_->setText(tr("Ready"));
    hdr_status_lbl_->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;").arg(col::POSITIVE()));
    status_label_->clear();
    update_llm_status();
}

// ── Re-translation ───────────────────────────────────────────────────────────

void AgentChatPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void AgentChatPanel::retranslateUi() {
    // Header.
    if (header_title_)
        header_title_->setText(tr("AGENT CHAT"));
    if (agent_caption_)
        agent_caption_->setText(tr("AGENT:"));
    if (agent_selector_) {
        agent_selector_->setToolTip(tr("Select a configured agent, or Default to use the global LLM."));
        if (agent_selector_->lineEdit())
            agent_selector_->lineEdit()->setPlaceholderText(tr("Search agent..."));
        // Item 0 is the fixed "Default (global LLM)" entry (empty data role).
        if (agent_selector_->count() > 0 && agent_selector_->itemData(0).toString().isEmpty())
            agent_selector_->setItemText(0, tr("Default (global LLM)"));
        const int e015_index = agent_selector_->findData(kE015Agent);
        if (e015_index >= 0)
            agent_selector_->setItemText(e015_index, tr("E015 策略AI（实时计划）"));
    }
    // Toggle buttons reflect on/off state — re-apply the matching label.
    if (route_toggle_)
        route_toggle_->setText(route_toggle_->isChecked() ? tr("AUTO-ROUTE: ON") : tr("AUTO-ROUTE"));
    if (route_toggle_)
        route_toggle_->setToolTip(tr("When ON, the system picks the best agent for each query."));
    if (run_as_task_toggle_)
        run_as_task_toggle_->setText(run_as_task_toggle_->isChecked() ? tr("RUN AS TASK: ON") : tr("RUN AS TASK"));
    if (run_as_task_toggle_)
        run_as_task_toggle_->setToolTip(
            tr("When ON, this query runs as a durable background task with per-step progress."));
    if (clear_btn_)
        clear_btn_->setText(tr("CLEAR"));
    if (hdr_model_lbl_)
        hdr_model_lbl_->setToolTip(tr("Active LLM — configure in Settings > LLM Configuration"));

    // Portfolio context bar.
    if (portfolio_caption_)
        portfolio_caption_->setText(tr("PORTFOLIO:"));
    // Item 0 is the fixed "None" entry (no data role).
    if (portfolio_combo_ && portfolio_combo_->count() > 0 && portfolio_combo_->itemData(0).toString().isEmpty())
        portfolio_combo_->setItemText(0, tr("None"));
    if (analyze_btn_)
        analyze_btn_->setText(tr("ANALYZE"));
    if (rebalance_btn_)
        rebalance_btn_->setText(tr("REBALANCE"));
    if (risk_btn_)
        risk_btn_->setText(tr("RISK"));

    // Welcome panel.
    if (welcome_title_)
        welcome_title_->setText(tr("How can I help you?"));
    if (welcome_subtitle_)
        welcome_subtitle_->setText(tr("Ask about markets, portfolios, or any financial topic.\n"
                                      "Select an agent above, or use Auto-Route to let the system decide."));

    // Input bar (send_btn_ flips to "..." while executing — leave that state).
    if (input_edit_)
        input_edit_->setPlaceholderText(tr("Message agent... (Shift+Enter for new line, Enter to send)"));
    if (send_btn_ && !executing_)
        send_btn_->setText(tr("Send"));

    // Header status pill + live status bar hold runtime state. Refresh the LLM
    // status (it re-derives Ready / model / Unconfigured text from current config).
    update_llm_status();
    update_e015_controls();
}

} // namespace fincept::screens
