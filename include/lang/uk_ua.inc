static const char *const g_uk_ua[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Перейти до налаштувань налагодження",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor і спільнота",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor і спільнота\nКерування: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: параметр recursive_scan=1 застарів; використовується scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: увімкнено глибину сканування %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Не вдалося змонтувати оверлей бекпорту: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "До гри %s застосовано емулятори",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "До гри %s застосовано бекпорт",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "До гри %s застосовано бекпорт\nЕмулятори оновлено",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Монтування образу відхилено:\n%s\nРозмір кластера замалий (%llu < %u).\nЗбережено перевизначення image_sector: %u.\nСпробуйте змонтувати знову.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Монтування образу відхилено:\n%s\nРозмір кластера замалий (%llu < %u).\nЗбільште розмір кластера в образі або зменште розмір сектора в конфігурації.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Не вдалося зареєструвати: %s (%s)\nAppInstallTitleDir недоступний",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Не вдалося зареєструвати: %s (%s)\nМіст TitleDir недоступний",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Не вдалося зареєструвати: %s (%s)\nкод=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Встановлення: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Встановлено: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Невідомо",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Не вдалося змонтувати образ: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Розмір кластера файлової системи (%llu) менший за розмір сектора пристрою (%u) для %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Змонтований образ не читається або пошкоджений",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Кеш образів заповнений (%u записів), неможливо відстежити монтування %s",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Не вдалося встановити: %s (%s)\nЧас очікування вичерпано через 5 хвилин",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Пакетне встановлення додано до черги (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... і ще %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Не вдалося пакетно встановити %d застосунків.\nкод=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "Файл param.json відсутній або некоректний:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Знайдено %d нових ігор. Обробка...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Бібліотеку синхронізовано.\nЗнайдено %d ігор.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: конфігурацію перезавантажено.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Дублікат %s проігноровано:\n%s\nнаявний: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Емулятор AMPR оновлено",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: не вдалося встановити хуки ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Цільова температура %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Зовнішній диск %s підключено.\nВільно %llu.%llu із %llu.%llu ГБ (блок %llu КБ)\nІгор на диску: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nДля максимальної продуктивності використовуйте кластер не більше 64 КБ",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Зовнішній диск %s відключено",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Гра %s аварійно завершилася.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Помилка у грі %s:\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "Зовнішній диск %s підключено.",
};
