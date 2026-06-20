static const char *const g_de_de[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */ [SM_L10N_ACTION_DEBUG_SETTINGS] = "Zu Debug-Einstellungen",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */ [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 ist veraltet, scan_depth=2 wird verwendet.",
    /* English: ShadowMount+: scan depth %u enabled. */ [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: Scan-Tiefe %u aktiviert.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */ [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-Overlay fehlgeschlagen: %s\n%s\n0x%08X",
    /* English: Game backported: %s */ [SM_L10N_GAME_BACKPORTED] = "Spiel backportiert: %s",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Image-Mount abgelehnt:\n%s\nClustergröße ist zu klein (%llu < %u).\nimage_sector-Override gespeichert: %u.\nErneut mounten.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Image-Mount abgelehnt:\n%s\nClustergröße ist zu klein (%llu < %u).\nVerwende eine größere Clustergröße im Image oder eine kleinere sector size in der Config.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */ [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registrierung fehlgeschlagen: %s (%s)\nAppInstallTitleDir nicht verfügbar",
    /* English: Register failed: %s (%s)\ncode=0x%08X */ [SM_L10N_REGISTER_FAILED_CODE] = "Registrierung fehlgeschlagen: %s (%s)\nCode=0x%08X",
    /* English: Installing: %s (%s)... */ [SM_L10N_INSTALLING] = "Installiere: %s (%s)...",
    /* English: Installed: %s */ [SM_L10N_INSTALLED] = "Installiert: %s",
    /* English: Image mount failed: 0x%08X (%s)\n%s */ [SM_L10N_IMAGE_MOUNT_FAILED] = "Image-Mount fehlgeschlagen: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Dateisystem-Clustergröße (%llu) ist kleiner als Gerätesektorgröße (%u) für %s",
    /* English: Mounted image is unreadable or damaged */ [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Gemountetes Image ist nicht lesbar oder beschädigt",
    /* English: Image cache full (%u entries), cannot track mount %s */ [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Image-Cache voll (%u Einträge), Mount %s kann nicht verfolgt werden",
    /* English: Image cache full (%u entries), rolling back mount */ [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Image-Cache voll (%u Einträge), Mount wird zurückgesetzt",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */ [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Installation fehlgeschlagen: %s (%s)\nZeitüberschreitung nach 5 Minuten",
    /* English: Batch install queued (%d): */ [SM_L10N_BATCH_INSTALL_QUEUED] = "Batch-Installation eingereiht (%d):",
    /* English: \n... and %d more */ [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... und %d weitere",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */ [SM_L10N_BATCH_INSTALL_FAILED] = "Batch-Installation für %d App(s) fehlgeschlagen.\nCode=0x%08X",
    /* English: Missing/invalid param.json:\n%s */ [SM_L10N_MISSING_PARAM] = "param.json fehlt oder ist ungültig:\n%s",
    /* English: Found %d new games. Executing... */ [SM_L10N_FOUND_NEW_GAMES] = "%d neue Spiele gefunden. Ausführung...",
    /* English: Library Synchronized.\nFound %d games. */ [SM_L10N_LIBRARY_SYNCED] = "Bibliothek synchronisiert.\n%d Spiele gefunden.",
    /* English: ShadowMount+: config reloaded. */ [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: Config neu geladen.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */ [SM_L10N_DUPLICATE_IGNORED] = "Duplikat %s ignoriert:\n%s\nvorhanden: %s",
};
