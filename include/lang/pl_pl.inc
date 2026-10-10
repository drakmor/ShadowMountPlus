static const char *const g_pl_pl[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Przejdź do ustawień debugowania",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor i społeczność",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor i społeczność\nSterowanie: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 jest przestarzałe, zastosowano scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: włączono głębokość skanowania %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Nakładka backportu nie powiodła się: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Zastosowano emulatory w grze: %s",
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
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Instalacja nie powiodła się: %s (%s)\nPrzekroczono limit czasu po 5 minutach",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Instalacja zbiorcza dodana do kolejki (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... i jeszcze %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Instalacja zbiorcza nie powiodła się dla %d aplikacji.\nkod=0x%08X",
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
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Temperatura docelowa %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Dysk zewnętrzny %s podłączony.\nWolne: %llu.%llu z %llu.%llu GB (rozmiar bloku %llu KB)\nGry na dysku: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nAby uzyskać maksymalną wydajność, użyj rozmiaru klastra nie większego niż 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Dysk zewnętrzny %s odłączony",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Gra %s uległa awarii.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Błąd w grze %s:\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "Dysk zewnętrzny %s podłączony.",
};
