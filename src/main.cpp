#include "mainwindow.hpp"
#include "kristall.hpp"

#include <QApplication>
#include <QUrl>
#include <QSettings>
#include <QCommandLineParser>
#include <QDebug>
#include <QStandardPaths>
#include <QFontDatabase>
#include <cassert>

ProtocolSetup       kristall::protocols;
IdentityCollection  kristall::identities;
QSettings *         kristall::settings;
QClipboard *        kristall::clipboard;
SslTrust            kristall::trust::gemini;
SslTrust            kristall::trust::https;
FavouriteCollection kristall::favourites;
GenericSettings     kristall::options;
DocumentStyle       kristall::document_style(false);
CacheHandler        kristall::cache;
QString             kristall::default_font_family;
QString             kristall::default_font_family_fixed;

QDir kristall::dirs::config_root;
QDir kristall::dirs::cache_root;
QDir kristall::dirs::offline_pages;
QDir kristall::dirs::themes;
QDir kristall::dirs::styles;

// We need QFont::setFamilies for emojis to work properly,
// Qt versions below 5.13 don't support this.
const bool kristall::EMOJIS_SUPPORTED =
#if QT_VERSION < QT_VERSION_CHECK(5, 13, 0)
    false;
#else
    true;
#endif

QString toFingerprintString(QSslCertificate const & certificate)
{
    return QCryptographicHash::hash(certificate.toDer(), QCryptographicHash::Sha256).toHex(':');
}

static QSettings * app_settings_ptr;
static QApplication * app;
static MainWindow * main_window = nullptr;
static bool closing_state_saved = false;

#define SSTR(X) STR(X)
#define STR(X) #X

static QDir derive_dir(QDir const & parent, QString const & subdir)
{
    QDir child = parent;
    if(not child.mkpath(subdir)) {
        qWarning() << "failed to initialize directory:" << subdir;
        return QDir { };
    }
    if(not child.cd(subdir)) {
        qWarning() << "failed to setup directory:" << subdir;
        return QDir { };
    }
    return child;
}

static void addEmojiSubstitutions()
{
    QFontDatabase db;

    auto const families = db.families();

    // Provide OpenMoji font for a safe fallback
    QFontDatabase::addApplicationFont(":/fonts/OpenMoji-Color.ttf");
    QFontDatabase::addApplicationFont(":/fonts/NotoColorEmoji.ttf");

    QStringList emojiFonts = {
        // Use system fonts on windows/mac
        "Apple Color Emoji",
        "Segoe UI Emoji",

        // Provide common fonts as a fallback:
        // "Noto Color Emoji", // this font seems to replace a lot of text characters?
        // "JoyPixels", // this font seems to replace a lot of text characters?

        // Built-in font fallback
        "OpenMoji",
    };

    for(auto const & family: families)
    {
        auto current = QFont::substitutes(family);
        current << emojiFonts;
        // TODO: QFont::insertSubstitutions(family, current);
    }
}


int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationVersion(SSTR(KRISTALL_VERSION));

    ::app = &app;

    {
        // Initialise default fonts
    #ifdef Q_OS_WIN32
        // Windows default fonts are ugly, so we use standard ones.
        kristall::default_font_family = "Segoe UI";
        kristall::default_font_family_fixed = "Consolas";
    #else
        // *nix
        kristall::default_font_family = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
        kristall::default_font_family_fixed = QFontInfo(QFont("monospace")).family();
    #endif
        kristall::document_style.initialiseDefaultFonts();
    }

    kristall::clipboard = app.clipboard();

    addEmojiSubstitutions();

    QCommandLineParser cli_parser;
    cli_parser.addVersionOption();
    cli_parser.addHelpOption();
    cli_parser.addPositionalArgument("urls", app.tr("The urls that should be opened instead of the start page"), "[urls...]");

    cli_parser.process(app);

    QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString config_root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

    kristall::dirs::config_root = QDir { config_root };
    kristall::dirs::cache_root  = QDir { cache_root };

    kristall::dirs::offline_pages = derive_dir(kristall::dirs::cache_root, "offline-pages");
    kristall::dirs::themes = derive_dir(kristall::dirs::config_root, "themes");

    kristall::dirs::styles = derive_dir(kristall::dirs::config_root, "styles");
    kristall::dirs::styles.setNameFilters(QStringList { "*.kthm" });
    kristall::dirs::styles.setFilter(QDir::Files);

    QSettings app_settings {
        kristall::dirs::config_root.absoluteFilePath("config.ini"),
                QSettings::IniFormat
    };
    app_settings_ptr = &app_settings;

    {
        QSettings deprecated_settings { "xqTechnologies", "Kristall" };
        if(QFile(deprecated_settings.fileName()).exists())
        {
            if(deprecated_settings.value("deprecated", false) == false)
            {
                qDebug() << "Migrating to new configuration style.";
                for(auto const & key : deprecated_settings.allKeys())
                {
                    app_settings.setValue(key, deprecated_settings.value(key));
                }

                // Migrate themes to new model
                {
                    int items = deprecated_settings.beginReadArray("Themes");

                    for(int i = 0; i < items; i++)
                    {
                        deprecated_settings.setArrayIndex(i);

                        QString name = deprecated_settings.value("name").toString();

                        DocumentStyle style;
                        style.load(deprecated_settings);

                        QString fileName;
                        int index = 0;
                        do {
                            fileName = DocumentStyle::createFileNameFromName(name, index);
                            index += 1;
                        } while(kristall::dirs::styles.exists(fileName));

                        QSettings style_sheet {
                            kristall::dirs::styles.absoluteFilePath(fileName),
                                    QSettings::IniFormat
                        };
                        style_sheet.setValue("name", name);
                        style.save(style_sheet);
                        style_sheet.sync();
                    }

                    deprecated_settings.endArray();
                }

                // Remove old theming stuff
                app_settings.remove("Theme");
                app_settings.remove("Themes");

                // Migrate "current theme" to new format
                {
                    DocumentStyle current_style;
                    deprecated_settings.beginGroup("Theme");
                    current_style.load(deprecated_settings);
                    deprecated_settings.endGroup();

                    app_settings.beginGroup("Theme");
                    current_style.save(app_settings);
                    app_settings.endGroup();
                }

                deprecated_settings.setValue("deprecated", true);
            }
            else
            {
                qDebug() << "Migration complete. Please delete" << deprecated_settings.fileName();
            }
        }
    }

    // Migrate to new favourites format
    if(int len = app_settings.beginReadArray("favourites"); len > 0)
    {
        qDebug() << "Migrating old-style favourites...";
        std::vector<Favourite> favs;

        favs.reserve(len);
        for(int i = 0; i < len; i++)
        {
            app_settings.setArrayIndex(i);

            Favourite fav;
            fav.destination = app_settings.value("url").toString();
            fav.title = QString { };

            favs.emplace_back(std::move(fav));
        }
        app_settings.endArray();


        app_settings.beginGroup("Favourites");
        {
            app_settings.beginWriteArray("groups");

            app_settings.setArrayIndex(0);
            app_settings.setValue("name", QObject::tr("Unsorted"));

            {
                app_settings.beginWriteArray("favourites", len);
                for(int i = 0; i < len; i++)
                {
                    auto const & fav = favs.at(i);
                    app_settings.setArrayIndex(i);
                    app_settings.setValue("title", fav.title);
                    app_settings.setValue("url", fav.destination);
                }
                app_settings.endArray();
            }

            app_settings.endArray();
        }
        app_settings.endGroup();

        app_settings.remove("favourites");
    }
    else {
        app_settings.endArray();
    }

    kristall::settings = &app_settings;

    kristall::options.load(app_settings);

    app_settings.beginGroup("Protocols");
    kristall::protocols.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Client Identities");
    kristall::identities.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted Servers");
    kristall::trust::gemini.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted HTTPS Servers");
    kristall::trust::https.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Theme");
    kristall::document_style.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Favourites");
    kristall::favourites.load(app_settings);
    app_settings.endGroup();

    kristall::setTheme(kristall::options.theme);

    MainWindow w(&app);
    main_window = &w;

    auto urls = cli_parser.positionalArguments();
    if(urls.size() > 0) {
        for(const auto &url_str : urls) {
            QUrl url { url_str };
            if (url.isRelative()) {
                if (QFile::exists(url_str)) {
                    url = QUrl::fromLocalFile(QFileInfo(url_str).absoluteFilePath());
                } else {
                    url = QUrl("gemini://" + url_str);
                }
            }
            if(url.isValid()) {
                w.addNewTab(false, url);
            } else {
                qDebug() << "Invalid url: " << url_str;
            }
        }
    }
    else {
        w.addEmptyTab(true, true);
    }

    app_settings.beginGroup("Window State");
    if(app_settings.contains("geometry")) {
        w.restoreGeometry(app_settings.value("geometry").toByteArray());
    }
    if(app_settings.contains("state")) {
        w.restoreState(app_settings.value("state").toByteArray());
    }
    app_settings.endGroup();

    w.show();

    int exit_code = app.exec();

    if (!closing_state_saved)
        kristall::saveWindowState();

    return exit_code;
}

void GenericSettings::load(QSettings &settings)
{
    network_timeout = settings.value("network_timeout", 5000).toInt();
    start_page = settings.value("start_page", "about:favourites").toString();
    search_engine = settings.value("search_engine", "gemini://geminispace.info/search?%1").toString();

    if(settings.value("text_display", "fancy").toString() == "plain")
        text_display = PlainText;
    else
        text_display = FormattedText;

    enable_text_decoration = settings.value("text_decoration", false).toBool();

    QString theme_name = settings.value("theme", "os_default").toString();
    if(theme_name == "dark")
        theme = Theme::dark;
    else if(theme_name == "light")
        theme = Theme::light;
    else
        theme = Theme::os_default;

    QString icon_theme_name = settings.value("icon_theme", "auto").toString();
    if (icon_theme_name == "light")
        icon_theme = IconTheme::light;
    else if (icon_theme_name == "dark")
        icon_theme = IconTheme::dark;
    else
        icon_theme = IconTheme::automatic;

    QString density = settings.value("ui_density", "compact").toString();
    if(density == "compact")
        ui_density = UIDensity::compact;
    else if (density == "classic")
        ui_density = UIDensity::classic;

    if(settings.value("gophermap_display", "rendered").toString() == "rendered")
        gophermap_display = FormattedText;
    else
        gophermap_display = PlainText;

    use_os_scheme_handler = settings.value("use_os_scheme_handler", false).toBool();

    show_hidden_files_in_dirs = settings.value("show_hidden_files_in_dirs", false).toBool();

    fancy_urlbar = settings.value("fancy_urlbar", true).toBool();

    fancy_quotes = settings.value("fancy_quotes", true).toBool();

    emojis_enabled = kristall::EMOJIS_SUPPORTED
        ? settings.value("emojis_enabled", true).toBool()
        : false;

    max_redirections = settings.value("max_redirections", 5).toInt();
    redirection_policy = RedirectionWarning(settings.value("redirection_policy ", WarnOnHostChange).toInt());

    enable_home_btn = settings.value("enable_home_btn", false).toBool();
    enable_newtab_btn = settings.value("enable_newtab_btn", true).toBool();
    enable_root_btn = settings.value("enable_root_btn", false).toBool();
    enable_parent_btn = settings.value("enable_parent_btn", false).toBool();

    cache_limit = settings.value("cache_limit", 1000).toInt();
    cache_threshold = settings.value("cache_threshold", 125).toInt();
    cache_life = settings.value("cache_life", 15).toInt();
    cache_unlimited_life = settings.value("cache_unlimited_life", true).toBool();
}

void GenericSettings::save(QSettings &settings) const
{
    settings.setValue("start_page", this->start_page);
    settings.setValue("search_engine", this->search_engine);
    settings.setValue("text_display", (text_display == FormattedText) ? "fancy" : "plain");
    settings.setValue("text_decoration", enable_text_decoration);
    QString theme_name = "os_default";
    switch(theme) {
    case Theme::dark:       theme_name = "dark"; break;
    case Theme::light:      theme_name = "light"; break;
    case Theme::os_default: theme_name = "os_default"; break;
    }
    settings.setValue("theme", theme_name);

    QString icon_theme_name = "auto";
    switch(icon_theme) {
    case IconTheme::dark:      icon_theme_name = "dark"; break;
    case IconTheme::light:     icon_theme_name = "light"; break;
    case IconTheme::automatic: icon_theme_name = "auto"; break;
    }
    settings.setValue("icon_theme", icon_theme_name);

    QString density = "compact";
    switch(ui_density) {
    case UIDensity::compact: density = "compact"; break;
    case UIDensity::classic: density = "classic"; break;
    }
    settings.setValue("ui_density", density);

    settings.setValue("gophermap_display", (gophermap_display == FormattedText) ? "rendered" : "text");
    settings.setValue("use_os_scheme_handler", use_os_scheme_handler);
    settings.setValue("show_hidden_files_in_dirs", show_hidden_files_in_dirs);
    settings.setValue("fancy_urlbar", fancy_urlbar);
    settings.setValue("fancy_quotes", fancy_quotes);
    settings.setValue("max_redirections", max_redirections);
    settings.setValue("redirection_policy", int(redirection_policy));
    settings.setValue("network_timeout", network_timeout);
    settings.setValue("enable_home_btn", enable_home_btn);
    settings.setValue("enable_newtab_btn", enable_newtab_btn);
    settings.setValue("enable_root_btn", enable_root_btn);
    settings.setValue("enable_parent_btn", enable_parent_btn);

    settings.setValue("cache_limit", cache_limit);
    settings.setValue("cache_threshold", cache_threshold);
    settings.setValue("cache_life", cache_life);
    settings.setValue("cache_unlimited_life", cache_unlimited_life);

    if (kristall::EMOJIS_SUPPORTED)
    {
        // Save emoji pref only if emojis are supported, so if user changes to a build
        // with emoji support, they get it out of the box.
        settings.setValue("emojis_enabled", emojis_enabled);
    }
}


void kristall::saveSettings()
{
    assert(app_settings_ptr != nullptr);
    QSettings & app_settings = *app_settings_ptr;

    app_settings.beginGroup("Favourites");
    kristall::favourites.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Protocols");
    kristall::protocols.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Client Identities");
    kristall::identities.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted Servers");
    kristall::trust::gemini.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted HTTPS Servers");
    kristall::trust::https.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Theme");
    kristall::document_style.save(app_settings);
    app_settings.endGroup();

    kristall::options.save(app_settings);

    app_settings.sync();
}

void kristall::setTheme(Theme theme)
{
    assert(app != nullptr);

    if(theme == Theme::os_default)
    {
        app->setStyleSheet("");

        // Use "mid" colour for our URL bar dim colour:
        QColor col = app->palette().color(QPalette::WindowText);
        col.setAlpha(150);
        kristall::options.fancy_urlbar_dim_colour = std::move(col);
    }
    else if(theme == Theme::light)
    {
        QFile file(":/light.qss");
        file.open(QFile::ReadOnly | QFile::Text);
        QTextStream stream(&file);
        app->setStyleSheet(stream.readAll());

        kristall::options.fancy_urlbar_dim_colour = QColor(128, 128, 128, 255);
    }
    else if(theme == Theme::dark)
    {
        QFile file(":/dark.qss");
        file.open(QFile::ReadOnly | QFile::Text);
        QTextStream stream(&file);
        app->setStyleSheet(stream.readAll());

        kristall::options.fancy_urlbar_dim_colour = QColor(150, 150, 150, 255);
    }

    kristall::setIconTheme(kristall::options.icon_theme, theme);

    if (main_window && main_window->curTab())
        main_window->curTab()->updateUrlBarStyle();
}

void kristall::setIconTheme(IconTheme icotheme, Theme uitheme)
{
    assert(app != nullptr);

    static const QString icothemes[] = {
        "light", // Light theme (dark icons)
        "dark"   // Dark theme (light icons)
    };

    auto ret = []() {
        if (main_window && main_window->curTab())
            main_window->curTab()->refreshToolbarIcons();
    };

    if (icotheme == IconTheme::automatic)
    {
        if (uitheme == Theme::os_default)
        {
            // For Linux we use standard system icon set,
            // for Windows & Mac we just use our default light theme icons.
        #if defined Q_OS_WIN32 || defined Q_OS_DARWIN
            QIcon::setThemeName("light");
        #else
            QIcon::setThemeName("");
        #endif

            kristall::options.explicit_icon_theme = IconTheme::dark;

            ret();
            return;
        }

        // Use icon theme based on UI theme
        QIcon::setThemeName(icothemes[(int)uitheme]);
        kristall::options.explicit_icon_theme = (IconTheme)uitheme;
        ret();
        return;
    }

    // Use icon specified by user
    QIcon::setThemeName(icothemes[(int)icotheme]);
    kristall::options.explicit_icon_theme = (IconTheme)icotheme;
    ret();
}

void kristall::setUiDensity(UIDensity density, bool previewing)
{
    assert(app != nullptr);
    assert(main_window != nullptr);
    main_window->setUiDensity(density, previewing);
}

void kristall::saveWindowState()
{
    closing_state_saved = true;

    app_settings_ptr->beginGroup("Window State");
    app_settings_ptr->setValue("geometry", main_window->saveGeometry());
    app_settings_ptr->setValue("state", main_window->saveState());
    app_settings_ptr->endGroup();

    kristall::saveSettings();
}
