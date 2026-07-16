static const char *const g_ru_ru[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Перейти к настройкам отладки",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: параметр recursive_scan=1 устарел; используется scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: включена глубина сканирования %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Не удалось смонтировать оверлей бэкпорта: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "К игре %s применён бэкпорт",
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
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff снова активен",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "Пока игра запущена, KStuff приостановлен",
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
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Кэш образов заполнен (%u записей), монтирование отменяется",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Сбой установки: %s (%s)\nПревышено время ожидания (5 минут)",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Пакетная установка добавлена в очередь (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... и ещё %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Сбой пакетной установки для %d приложений.\nкод=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Приложение аварийно завершилось до приостановки KStuff: %s. Причина не в KStuff.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "Приложение %s аварийно завершилось до приостановки KStuff:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Приложение аварийно завершилось после приостановки KStuff: %s. Автоподбор задержки пропущен.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "Приложение %s аварийно завершилось после приостановки KStuff:\n%s\nАвтоподбор задержки пропущен.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Не удалось загрузить модуль после приостановки KStuff",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "не удалось загрузить модуль %.*s после приостановки KStuff",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Задержка увеличена до %u с.\nЗапустите игру снова.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Обнаружен сбой после приостановки KStuff: задержка для %s увеличена до %u с. Запустите игру снова.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "Приложение %s аварийно завершилось после приостановки KStuff:\n%s\nЗадержка увеличена до %u с.\nЗапустите игру снова.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Обнаружен сбой после приостановки KStuff: %s. Не удалось обновить задержку автоподбора.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "Приложение %s аварийно завершилось после приостановки KStuff:\n%s\nНе удалось обновить задержку автоподбора.",
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
};
