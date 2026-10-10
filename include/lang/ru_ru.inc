static const char *const g_ru_ru[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Перейти к настройкам отладки",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor и Сообщество",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor и Сообщество\nУправление: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: параметр recursive_scan=1 устарел; используется scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: включена глубина сканирования %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Не удалось смонтировать оверлей бэкпорта: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "К игре %s применён бэкпорт",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "К игре %s применён бэкпорт\nЭмуляторы обновлены",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Не удалось смонтировать образ:\n%s\nРазмер кластера слишком мал (%llu < %u).\nСохранено значение image_sector: %u.\nПопробуйте смонтировать снова.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Не удалось смонтировать образ:\n%s\nРазмер кластера слишком мал (%llu < %u).\nУвеличьте размер кластера в образе или уменьшите размер сектора в конфигурации.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Не удалось зарегистрировать: %s (%s)\nAppInstallTitleDir недоступен",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Не удалось зарегистрировать: %s (%s)\nМост TitleDir недоступен",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Не удалось зарегистрировать: %s (%s)\nкод=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Устанавливается: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Установлено: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Неизвестно",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Не удалось смонтировать образ: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Размер кластера файловой системы (%llu) меньше размера сектора устройства (%u) для %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Смонтированный образ не читается или повреждён",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Кэш образов заполнен (%u записей), невозможно отследить монтирование %s",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Сбой установки: %s (%s)\nПревышено время ожидания (5 минут)",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Пакетная установка добавлена в очередь (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... и ещё %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Сбой пакетной установки для %d приложений.\nкод=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "Отсутствует или некорректен файл param.json:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Найдено %d новых игр.\nВыполняется обновление библиотеки.",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Библиотека обновлена\nНайдено %d игр(ы)",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: конфигурация перезагружена.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Дубликат %s пропущен:\n%s\nуже существует: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Эмулятор AMPR обновлён",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: не удалось установить хуки ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Целевая температура %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Внешний диск %s подключён.\nСвободно %llu.%llu из %llu.%llu ГБ (блок %llu КБ)\nИгр на диске: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nДля максимальной производительности используйте кластер не более 64 КБ",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Внешний диск %s отключён.",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Игра %s аварийно завершилась.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Ошибка в игре %s:\n%s",
};
