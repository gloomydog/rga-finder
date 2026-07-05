// rga-gui (C++/Qt): a desktop app that uses rga (ripgrep-all) under the hood to
//   show every occurrence of a search term, one sentence at a time, with a page
//   or position badge, in a native window. No browser needed. Native to KDE Plasma.
//
//   Languages: English, Japanese, Chinese, French, Italian, German, Spanish
//   (whole-word -w is auto-disabled for CJK). Searches across multiple folders; for
//   PDFs it rebuilds full sentences. Results shown as a collapsible per-file list.
//   Each file shows its parent directory, and can be re-searched on its own.
//
// ---- Build (Arch) ---------------------------------------------------------
//   sudo pacman -S qt6-base
//   g++ -std=c++17 -fPIC rga_gui.cpp -o rga-gui
//       $(pkg-config --cflags --libs Qt6Widgets)
//   ./rga-gui
//
//   If pkg-config cannot find Qt6, use CMake instead:
//     cmake_minimum_required(VERSION 3.16)
//     project(rga_gui LANGUAGES CXX)
//     set(CMAKE_CXX_STANDARD 17)
//     find_package(Qt6 REQUIRED COMPONENTS Widgets)
//     add_executable(rga-gui rga_gui.cpp)
//     target_link_libraries(rga-gui PRIVATE Qt6::Widgets)
//
// ---- Runtime dependencies -------------------------------------------------
//   ripgrep-all / pandoc-cli (for EPUB) / poppler (for PDF)
// ---------------------------------------------------------------------------

#include <QApplication>
#include <QWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QCheckBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSizePolicy>
#include <QProcess>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QVector>
#include <QStringList>
#include <functional>
#include <memory>
#include <algorithm>
#include <cmath>

// -------------------------------------------------------------- Data types
struct Hit {
    int page = -1;        // PDF page (-1 if none)
    int percent = -1;     // position % within the book, for EPUB etc. (-1 if none)
    qint64 offset = -1;
    QString sentence;
};
struct RenderedBook {
    QString title;
    QString path;         // full file path (used to search this file alone)
    QString dirDisplay;   // parent directory, home-abbreviated
    int count = 0;
    QString hitsHtml;
};
struct BuildResult { QVector<RenderedBook> books; int total = 0; bool skipped = false; };

// -------------------------------------------------------------- Language detection
static bool isCJK(const QString &w) {
    for (const QChar &ch : w) {
        const ushort u = ch.unicode();
        if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x3400 && u <= 0x4DBF) ||
            (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) ||
            (u >= 0x3000 && u <= 0x303F))
            return true;
    }
    return false;
}

// Add word boundaries \b only when wholeWord is set (so partial searches still highlight).
static QRegularExpression makeWordRegex(const QString &word, bool wholeWord) {
    const QString esc = QRegularExpression::escape(word);
    const QString pat = wholeWord ? ("\\b(" + esc + ")\\b") : ("(" + esc + ")");
    return QRegularExpression(pat, QRegularExpression::CaseInsensitiveOption
    | QRegularExpression::UseUnicodePropertiesOption);
}

// -------------------------------------------------------------- Helpers
static bool isTerm(QChar c) {
    static const QString T = QStringLiteral(".!?。！？");
    return T.contains(c);
}

static QString extractSentenceAround(const QString &s, int wStart, int wEnd) {
    int left = wStart;
    while (left > 0 && !isTerm(s.at(left - 1))) --left;
    int right = wEnd;
    while (right < s.length() && !isTerm(s.at(right))) ++right;
    if (right < s.length()) ++right;
    return s.mid(left, right - left).trimmed();
}

static QString highlight(const QString &text, const QRegularExpression &wre) {
    QString safe = text.toHtmlEscaped();
    safe.replace(wre, QStringLiteral("<span style=\"background-color:#ffe08a\">\\1</span>"));
    return safe;
}

static void niceTitle(const QString &path, QString &title, QString &author) {
    static const QRegularExpression extRe(
        QStringLiteral("\\.(epub|pdf|mobi|azw3?|txt|docx?|fb2|odt)$"),
                                          QRegularExpression::CaseInsensitiveOption);
    QString base = QFileInfo(path).fileName();
    base.remove(extRe);
    const QStringList parts = base.split(QStringLiteral(" -- "));
    title = parts.value(0).trimmed();
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    author.clear();
    if (parts.size() > 1) { author = parts.value(1).trimmed(); author.remove(QLatin1Char('_')); }
}

// Parent directory of a file, with the home directory shown as "~".
static QString dirDisplayOf(const QString &path) {
    QString parent = QFileInfo(path).absolutePath();
    const QString home = QDir::homePath();
    if (parent == home) return QStringLiteral("~");
    if (parent.startsWith(home + QLatin1Char('/')))
        parent = QStringLiteral("~") + parent.mid(home.length());
    return parent;
}

static void clearLayout(QLayout *lay) {
    QLayoutItem *it;
    while ((it = lay->takeAt(0)) != nullptr) {
        if (QWidget *w = it->widget()) w->deleteLater();
        delete it;
    }
}

// -------------------------------------------------------------- Parsing rga output
static BuildResult buildResults(const QByteArray &out, const QByteArray &err,
                                const QString &word, bool wholeWord) {
    const QRegularExpression wre = makeWordRegex(word, wholeWord);
    static const QRegularExpression pageRe(QStringLiteral("^Page (\\d+):\\s?(.*)$"));
    const int R = 3;

    struct RawMatch { QString path; int line; int page; qint64 offset; QString matchText; };
    QVector<RawMatch> rawMatches;
    QMap<QString, QMap<int, QString>> fileLines;
    QMap<QString, qint64> bytesByPath;
    QStringList order;

    for (const QByteArray &lineRaw : out.split('\n')) {
        if (lineRaw.trimmed().isEmpty()) continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(lineRaw, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const QJsonObject o = doc.object();
        const QString type = o.value(QStringLiteral("type")).toString();
        const QJsonObject data = o.value(QStringLiteral("data")).toObject();
        const QString path = data.value(QStringLiteral("path")).toObject()
        .value(QStringLiteral("text")).toString();

        if (type == QLatin1String("match") || type == QLatin1String("context")) {
            QString raw = data.value(QStringLiteral("lines")).toObject()
            .value(QStringLiteral("text")).toString();
            while (raw.endsWith(QLatin1Char('\n')) || raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
            int page = -1; QString text;
            const auto m = pageRe.match(raw);
            if (m.hasMatch()) { page = m.captured(1).toInt(); text = m.captured(2); }
            else              { text = raw; }
            const int lineNo = data.value(QStringLiteral("line_number")).toInt(-1);
            if (lineNo >= 0) fileLines[path][lineNo] = text;
            if (type == QLatin1String("match")) {
                if (!order.contains(path)) order << path;
                RawMatch rm;
                rm.path = path; rm.line = lineNo; rm.page = page;
                rm.offset = data.value(QStringLiteral("absolute_offset")).toInteger(-1);
                rm.matchText = text;
                rawMatches.append(rm);
            }
        } else if (type == QLatin1String("end")) {
            bytesByPath[path] = data.value(QStringLiteral("stats")).toObject()
            .value(QStringLiteral("bytes_searched")).toInteger(0);
        }
    }

    BuildResult result;
    result.skipped = QString::fromUtf8(err).contains(QStringLiteral("Could not find executable"));

    struct TmpBook { QString path, title, author; QVector<Hit> hits; };
    QVector<TmpBook> tmp;
    for (const QString &path : order) {
        TmpBook b; b.path = path;
        niceTitle(path, b.title, b.author);
        const QMap<int, QString> &lm = fileLines.value(path);
        const qint64 bytes = bytesByPath.value(path, 0);
        for (const RawMatch &rm : rawMatches) {
            if (rm.path != path) continue;
            QString matchText = !rm.matchText.isEmpty() ? rm.matchText : lm.value(rm.line);
            bool hasTerm = false;
            for (const QChar &c : matchText) if (isTerm(c)) { hasTerm = true; break; }

            QString full = matchText;
            int segStart = 0, segEnd = matchText.length();
            if (!hasTerm) {
                QStringList beforeParts, afterParts;
                for (int i = rm.line - R; i <= rm.line - 1; ++i) if (lm.contains(i)) beforeParts << lm.value(i);
                for (int i = rm.line + 1; i <= rm.line + R; ++i) if (lm.contains(i)) afterParts << lm.value(i);
                const QString before = beforeParts.join(QLatin1Char(' '));
                const QString after  = afterParts.join(QLatin1Char(' '));
                full.clear();
                if (!before.isEmpty()) { full = before + QLatin1Char(' '); segStart = full.length(); }
                full += matchText;
                segEnd = full.length();
                if (!after.isEmpty()) full += QLatin1Char(' ') + after;
            }

            int ws, we;
            QRegularExpressionMatch mm = wre.match(full, segStart);
            if (mm.hasMatch() && mm.capturedStart(1) < segEnd) { ws = mm.capturedStart(1); we = mm.capturedEnd(1); }
            else {
                QRegularExpressionMatch mm2 = wre.match(full);
                if (mm2.hasMatch()) { ws = mm2.capturedStart(1); we = mm2.capturedEnd(1); }
                else { ws = segStart; we = segEnd; }
            }

            Hit h; h.page = rm.page; h.offset = rm.offset;
            h.sentence = extractSentenceAround(full, ws, we);
            if (h.sentence.trimmed().isEmpty()) h.sentence = matchText.trimmed();
            if (h.page < 0 && bytes > 0 && h.offset >= 0) {
                const int p = int(std::lround(double(h.offset) * 100.0 / double(bytes)));
                h.percent = std::max(0, std::min(100, p));
            }
            b.hits.append(h);
        }
        std::sort(b.hits.begin(), b.hits.end(), [](const Hit &a, const Hit &c) {
            const bool an = a.page < 0, cn = c.page < 0;
            if (an != cn) return !an;
            const int av = an ? a.percent : a.page;
            const int cv = cn ? c.percent : c.page;
            return av < cv;
        });
        tmp.append(b);
    }
    std::sort(tmp.begin(), tmp.end(),
              [](const TmpBook &a, const TmpBook &c){ return a.hits.size() > c.hits.size(); });

    for (const TmpBook &b : tmp) {
        RenderedBook rb;
        rb.title = b.title;
        rb.path = b.path;
        rb.dirDisplay = dirDisplayOf(b.path);
        rb.count = b.hits.size();
        QString html = QStringLiteral("<div style=\"color:#2b2622;\">");
        if (!b.author.isEmpty())
            html += QStringLiteral("<div style=\"color:#8a8178;margin:2px 0 8px;\"><small>%1</small></div>")
            .arg(b.author.toHtmlEscaped());
        for (const Hit &h : b.hits) {
            QString badge;
            if (h.page >= 0)         badge = QStringLiteral("p.%1").arg(h.page);
            else if (h.percent >= 0) badge = QStringLiteral("%1%").arg(h.percent);
            const QString badgeHtml = badge.isEmpty() ? QString()
            : QStringLiteral("<span style=\"background-color:#efe7d6;color:#8a5a2b;"
            "\">&nbsp;%1&nbsp;</span> ").arg(badge);
            html += QStringLiteral(
                "<p style=\"margin:8px 0;font-family:Georgia,serif;font-size:15px;\">%1%2</p>")
            .arg(badgeHtml, highlight(h.sentence, wre));
        }
        html += QStringLiteral("</div>");
        rb.hitsHtml = html;
        result.total += rb.count;
        result.books.append(rb);
    }
    return result;
                                }

                                // -------------------------------------------------------------- main
                                int main(int argc, char *argv[]) {
                                    QApplication app(argc, argv);

                                    QWidget window;
                                    window.setWindowTitle(QStringLiteral("rga Example Finder"));
                                    window.resize(900, 720);
                                    auto *root = new QVBoxLayout(&window);

                                    // --- Search term row ---
                                    auto *row1 = new QHBoxLayout();
                                    auto *wordEdit = new QLineEdit();
                                    wordEdit->setPlaceholderText(QStringLiteral("Search word (any language)"));
                                    auto *searchBtn = new QPushButton(QStringLiteral("Search"));
                                    auto *wbCheck = new QCheckBox(QStringLiteral("Whole word (-w)")); wbCheck->setChecked(true);
                                    wbCheck->setToolTip(QStringLiteral("Ignored automatically for Japanese/Chinese"));
                                    auto *icCheck = new QCheckBox(QStringLiteral("Ignore case (-i)")); icCheck->setChecked(true);
                                    row1->addWidget(wordEdit, 1);
                                    row1->addWidget(searchBtn);
                                    row1->addWidget(wbCheck);
                                    row1->addWidget(icCheck);
                                    root->addLayout(row1);

                                    // --- Folders and files (one per line) ---
                                    root->addWidget(new QLabel(QStringLiteral("Search folders and files (one per line):")));
                                    auto *dirRow = new QHBoxLayout();
                                    auto *dirEdit = new QPlainTextEdit(QDir::homePath() + QStringLiteral("/Downloads/books"));
                                    dirEdit->setMaximumHeight(78);
                                    auto *btnCol = new QVBoxLayout();
                                    auto *addBtn = new QPushButton(QStringLiteral("Add folder…"));
                                    auto *addFileBtn = new QPushButton(QStringLiteral("Add file…"));
                                    btnCol->addWidget(addBtn);
                                    btnCol->addWidget(addFileBtn);
                                    btnCol->addStretch();
                                    dirRow->addWidget(dirEdit, 1);
                                    dirRow->addLayout(btnCol);
                                    root->addLayout(dirRow);

                                    // --- Status + expand/collapse-all button ---
                                    auto *toolRow = new QHBoxLayout();
                                    auto *status = new QLabel(QStringLiteral(" "));
                                    status->setStyleSheet(QStringLiteral("color:#8a8178;"));
                                    auto *toggleAllBtn = new QPushButton(QStringLiteral("Collapse all"));
                                    toggleAllBtn->setEnabled(false);
                                    toolRow->addWidget(status, 1);
                                    toolRow->addWidget(toggleAllBtn);
                                    root->addLayout(toolRow);

                                    // --- Results (scrollable accordion) ---
                                    auto *scroll = new QScrollArea();
                                    scroll->setWidgetResizable(true);
                                    scroll->setStyleSheet(QStringLiteral("QScrollArea{background:#fffdf8;border:1px solid #e4ddd0;}"));
                                    auto *resultsContainer = new QWidget();
                                    auto *resultsLayout = new QVBoxLayout(resultsContainer);
                                    resultsLayout->setAlignment(Qt::AlignTop);
                                    scroll->setWidget(resultsContainer);
                                    root->addWidget(scroll, 1);

                                    auto *proc = new QProcess(&window);
                                    auto headers = std::make_shared<QVector<QToolButton*>>();

                                    auto appendPath = [=](const QString &p) {
                                        if (p.isEmpty()) return;
                                        QString t = dirEdit->toPlainText();
                                        if (!t.isEmpty() && !t.endsWith(QLatin1Char('\n'))) t += QLatin1Char('\n');
                                        dirEdit->setPlainText(t + p);
                                    };

                                    QObject::connect(addBtn, &QPushButton::clicked, [=]() {
                                        appendPath(QFileDialog::getExistingDirectory(dirEdit, QStringLiteral("Add folder"), QDir::homePath()));
                                    });
                                    QObject::connect(addFileBtn, &QPushButton::clicked, [=]() {
                                        appendPath(QFileDialog::getOpenFileName(
                                            dirEdit, QStringLiteral("Add file"), QDir::homePath(),
                                                                                QStringLiteral("Books (*.pdf *.epub *.mobi *.azw3 *.txt);;All files (*)")));
                                    });

                                    // Run rga against an explicit list of paths (folders and/or single files).
                                    std::function<void(const QStringList&)> runSearch = [=](const QStringList &paths) {
                                        const QString word = wordEdit->text().trimmed();
                                        if (word.isEmpty()) return;
                                        if (paths.isEmpty()) { status->setText(QStringLiteral("Add at least one folder or file.")); return; }
                                        QStringList args;
                                        args << QStringLiteral("--json") << QStringLiteral("--context") << QStringLiteral("3");
                                        if (!isCJK(word) && wbCheck->isChecked()) args << QStringLiteral("-w");
                                        if (icCheck->isChecked()) args << QStringLiteral("-i");
                                        args << QStringLiteral("--") << word;
                                        for (const QString &p : paths) args << p;
                                        searchBtn->setEnabled(false);
                                        status->setText(QStringLiteral("Searching…"));
                                        proc->start(QStringLiteral("rga"), args);
                                    };

                                    // Collect the paths typed in the box (each line is a folder or a file).
                                    auto gatherPaths = [=]() -> QStringList {
                                        QStringList paths;
                                        for (QString d : dirEdit->toPlainText().split(QLatin1Char('\n'))) {
                                            d = d.trimmed();
                                            if (d.isEmpty()) continue;
                                            if (d.startsWith(QLatin1Char('~'))) d = QDir::homePath() + d.mid(1);
                                            paths << d;
                                        }
                                        return paths;
                                    };

                                    auto doSearch = [=]() { runSearch(gatherPaths()); };
                                    QObject::connect(searchBtn, &QPushButton::clicked, doSearch);
                                    QObject::connect(wordEdit, &QLineEdit::returnPressed, doSearch);

                                    QObject::connect(toggleAllBtn, &QPushButton::clicked, [=]() {
                                        bool anyOpen = false;
                                        for (auto *h : *headers) if (h->isChecked()) { anyOpen = true; break; }
                                        for (auto *h : *headers) h->setChecked(!anyOpen);
                                        toggleAllBtn->setText(anyOpen ? QStringLiteral("Expand all")
                                        : QStringLiteral("Collapse all"));
                                    });

                                    QObject::connect(proc, &QProcess::errorOccurred, [=](QProcess::ProcessError e) {
                                        if (e == QProcess::FailedToStart) {
                                            status->setText(QStringLiteral("rga not found. Install: sudo pacman -S ripgrep-all"));
                                            searchBtn->setEnabled(true);
                                        }
                                    });

                                    QObject::connect(proc, &QProcess::finished, [=](int code, QProcess::ExitStatus) {
                                        const QByteArray out = proc->readAllStandardOutput();
                                        const QByteArray err = proc->readAllStandardError();
                                        searchBtn->setEnabled(true);

                                        clearLayout(resultsLayout);
                                        headers->clear();

                                        if (code == 2 && out.trimmed().isEmpty()) {
                                            status->setText(QStringLiteral("Error: ") + QString::fromUtf8(err).trimmed());
                                            toggleAllBtn->setEnabled(false);
                                            return;
                                        }

                                        const QString w = wordEdit->text().trimmed();
                                        const bool ww = wbCheck->isChecked() && !isCJK(w);
                                        const BuildResult r = buildResults(out, err, w, ww);
                                        status->setText(QStringLiteral("%1 matches / %2 books").arg(r.total).arg(r.books.size()));

                                        if (r.skipped) {
                                            auto *warn = new QLabel(QStringLiteral(
                                                "Some files were skipped (pandoc/poppler may be missing)."));
                                            warn->setWordWrap(true);
                                            warn->setStyleSheet(QStringLiteral("color:#8a5a2b;background:#fff4e0;padding:6px;"));
                                            resultsLayout->addWidget(warn);
                                        }
                                        if (r.books.isEmpty()) {
                                            auto *none = new QLabel(QStringLiteral("No matches"));
                                            none->setStyleSheet(QStringLiteral("color:#8a8178;padding:8px;"));
                                            resultsLayout->addWidget(none);
                                            toggleAllBtn->setEnabled(false);
                                            return;
                                        }

                                        for (const RenderedBook &rb : r.books) {
                                            // File header: toggle button + "Only this file" button + directory line.
                                            auto *headerWidget = new QWidget();
                                            auto *hv = new QVBoxLayout(headerWidget);
                                            hv->setContentsMargins(0, 0, 0, 0);
                                            hv->setSpacing(0);

                                            auto *topRow = new QHBoxLayout();
                                            topRow->setContentsMargins(0, 0, 0, 0);

                                            auto *header = new QToolButton();
                                            header->setText(QStringLiteral("%1  (%2)").arg(rb.title).arg(rb.count));
                                            header->setCheckable(true);
                                            header->setChecked(true);
                                            header->setArrowType(Qt::DownArrow);
                                            header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                                            header->setAutoRaise(true);
                                            header->setCursor(Qt::PointingHandCursor);
                                            header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                                            header->setStyleSheet(QStringLiteral(
                                                "QToolButton{font-weight:bold;font-size:14px;border:none;"
                                                "padding:8px 4px;text-align:left;color:#2b2622;}"
                                                "QToolButton:hover{background:#f4efe4;}"));

                                            auto *onlyBtn = new QToolButton();
                                            onlyBtn->setText(QStringLiteral("Only this file"));
                                            onlyBtn->setAutoRaise(true);
                                            onlyBtn->setCursor(Qt::PointingHandCursor);
                                            onlyBtn->setToolTip(QStringLiteral("Search the current word in this file only"));
                                            onlyBtn->setStyleSheet(QStringLiteral(
                                                "QToolButton{color:#8a5a2b;border:1px solid #e4ddd0;border-radius:4px;padding:3px 8px;}"
                                                "QToolButton:hover{background:#f4efe4;}"));
                                            const QString filePath = rb.path;
                                            QObject::connect(onlyBtn, &QToolButton::clicked,
                                                             [runSearch, filePath]() { runSearch(QStringList{filePath}); });

                                            topRow->addWidget(header, 1);
                                            topRow->addWidget(onlyBtn, 0, Qt::AlignVCenter);
                                            hv->addLayout(topRow);

                                            auto *dirLabel = new QLabel(rb.dirDisplay);
                                            dirLabel->setStyleSheet(QStringLiteral("color:#8a8178;font-size:11px;padding:0 0 4px 22px;"));
                                            hv->addWidget(dirLabel);

                                            auto *content = new QLabel(rb.hitsHtml);
                                            content->setTextFormat(Qt::RichText);
                                            content->setWordWrap(true);
                                            content->setTextInteractionFlags(Qt::TextSelectableByMouse);
                                            content->setStyleSheet(QStringLiteral("QLabel{padding:0 12px 8px 22px;}"));

                                            QObject::connect(header, &QToolButton::toggled, [content, header](bool on) {
                                                content->setVisible(on);
                                                header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
                                            });

                                            resultsLayout->addWidget(headerWidget);
                                            resultsLayout->addWidget(content);
                                            headers->append(header);
                                        }
                                        toggleAllBtn->setEnabled(true);
                                        toggleAllBtn->setText(QStringLiteral("Collapse all"));
                                    });

                                    window.show();
                                    return app.exec();
                                }
