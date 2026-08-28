static const char *const g_de_de[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Zu den Debug-Einstellungen",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor und die Community",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 ist veraltet, scan_depth=2 wird verwendet.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: Scan-Tiefe %u ist aktiviert.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-Overlay fehlgeschlagen: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Spiel backportiert: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Spiel backportiert: %s\nEmulatoren aktualisiert",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Einbindung des Images abgelehnt:\n%s\nDie Clustergröße ist zu klein (%llu < %u).\nimage_sector-Überschreibung gespeichert: %u.\nVersuche es erneut.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Einbindung des Images abgelehnt:\n%s\nDie Clustergröße ist zu klein (%llu < %u).\nVerwende im Image eine größere Clustergröße oder in der Konfiguration eine kleinere Sektorgröße.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registrierung fehlgeschlagen: %s (%s)\nAppInstallTitleDir nicht verfügbar",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Registrierung fehlgeschlagen: %s (%s)\nTitleDir-Bridge nicht verfügbar",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registrierung fehlgeschlagen: %s (%s)\nCode=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Installiere: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff ist wieder aktiv",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff pausiert, während das Spiel läuft",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Installiert: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Unbekannt",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Einbindung des Images fehlgeschlagen: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Dateisystem-Clustergröße (%llu) ist kleiner als Gerätesektorgröße (%u) für %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Gemountetes Image ist nicht lesbar oder beschädigt",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Image-Cache voll (%u Einträge), Einbindung %s kann nicht verfolgt werden",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Image-Cache voll (%u Einträge), Einbindung wird rückgängig gemacht",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Installation fehlgeschlagen: %s (%s)\nZeitüberschreitung nach 5 Minuten",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Batch-Installation eingereiht (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... und %d weitere",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Batch-Installation für %d Apps fehlgeschlagen.\nCode=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "%s ist abgestürzt, bevor KStuff pausiert wurde. KStuff ist nicht die Ursache.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s ist vor der KStuff-Pause abgestürzt:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "%s ist abgestürzt, nachdem KStuff pausiert wurde. Die automatische Abstimmung wurde übersprungen.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s ist nach der KStuff-Pause abgestürzt:\n%s\nAutomatische Abstimmung übersprungen.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Modul kann nach KStuff-Pause nicht geladen werden",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "Modul %.*s kann nach der KStuff-Pause nicht geladen werden",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Verzögerung auf %u s erhöht.\nStarte das Spiel erneut.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Absturz nach der KStuff-Pause erkannt: Die Pausenverzögerung für %s wurde auf %u s erhöht. Starte das Spiel erneut.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s ist nach der KStuff-Pause abgestürzt:\n%s\nVerzögerung auf %u s erhöht.\nStarte das Spiel erneut.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Absturz nach der KStuff-Pause erkannt: %s. Die Verzögerung der automatischen Abstimmung konnte nicht aktualisiert werden.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s ist nach der KStuff-Pause abgestürzt:\n%s\nVerzögerung der automatischen Abstimmung konnte nicht aktualisiert werden.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json fehlt oder ist ungültig:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d neue Spiele gefunden. Verarbeitung läuft...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliothek synchronisiert.\n%d Spiele gefunden.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: Konfiguration neu geladen.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplikat %s ignoriert:\n%s\nvorhanden: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR-Emulator aktualisiert",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: ShellCore-Hooks konnten nicht installiert werden",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Zieltemperatur %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Externes Laufwerk %s verbunden.\nFrei: %llu.%llu von %llu.%llu GB (Blockgröße %llu KB)\nSpiele auf dem Laufwerk: %d%s",
    /* English: For maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nVerwenden Sie für maximale Leistung eine Clustergröße von höchstens 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Externes Laufwerk %s getrennt",
};
