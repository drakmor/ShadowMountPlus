static const char *const g_pl_pl[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Przejdź do ustawień debugowania",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 jest przestarzałe, zastosowano scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: włączono głębokość skanowania %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Nakładka backportu nie powiodła się: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Zastosowano backport gry: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Zastosowano backport gry: %s\nEmulatory zaktualizowane",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Odrzucono montowanie obrazu:\n%s\nRozmiar klastra jest za mały (%llu < %u).\nZastosowano nadpisanie image_sector: %u.\nSpróbuj zamontować ponownie.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Odrzucono montowanie obrazu:\n%s\nRozmiar klastra jest za mały (%llu < %u).\nUżyj większego rozmiaru klastra w obrazie lub mniejszego rozmiaru sektora w konfiguracji.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Rejestracja nie powiodła się: %s (%s)\nAppInstallTitleDir niedostępne",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Rejestracja nie powiodła się: %s (%s)\nMost TitleDir jest niedostępny",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Rejestracja nie powiodła się: %s (%s)\nkod=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Instalowanie: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff ponownie aktywny",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff wstrzymany podczas działania gry",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Zainstalowano: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Nieznane",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Montowanie obrazu nie powiodło się: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Rozmiar klastra systemu plików (%llu) jest mniejszy niż rozmiar sektora urządzenia (%u) dla %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Zamontowany obraz jest nieczytelny lub uszkodzony",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Pamięć podręczna obrazów jest pełna (%u wpisów), nie można śledzić montowania %s",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Pamięć podręczna obrazów jest pełna (%u wpisów), montowanie zostanie wycofane",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Instalacja nie powiodła się: %s (%s)\nPrzekroczono limit czasu po 5 minutach",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Instalacja zbiorcza dodana do kolejki (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... i jeszcze %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Instalacja zbiorcza nie powiodła się dla %d aplikacji.\nkod=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Aplikacja %s uległa awarii przed wstrzymaniem KStuff. KStuff nie jest przyczyną.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "Aplikacja %s uległa awarii przed wstrzymaniem KStuff:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Aplikacja %s uległa awarii po wstrzymaniu KStuff. Pominięto automatyczne dostrajanie.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "Aplikacja %s uległa awarii po wstrzymaniu KStuff:\n%s\nPominięto automatyczne dostrajanie.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Nie można załadować modułu po wstrzymaniu KStuff",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "nie można załadować modułu %.*s po wstrzymaniu KStuff",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Opóźnienie zwiększono do %u s.\nUruchom grę ponownie.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Wykryto awarię po wstrzymaniu KStuff: opóźnienie dla %s zwiększono do %u s. Uruchom grę ponownie.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "Aplikacja %s uległa awarii po wstrzymaniu KStuff:\n%s\nOpóźnienie zwiększono do %u s.\nUruchom grę ponownie.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Wykryto awarię po wstrzymaniu KStuff: %s. Nie udało się zaktualizować opóźnienia automatycznego dostrajania.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "Aplikacja %s uległa awarii po wstrzymaniu KStuff:\n%s\nNie udało się zaktualizować opóźnienia automatycznego dostrajania.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "Brak pliku param.json lub plik jest nieprawidłowy:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Znaleziono %d nowych gier. Przetwarzanie...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Biblioteka zsynchronizowana.\nZnaleziono %d gier.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: konfiguracja przeładowana.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplikat %s zignorowany:\n%s\nistniejący: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Emulator AMPR został zaktualizowany",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: nie udało się zainstalować hooków ShellCore",
};
