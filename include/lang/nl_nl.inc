static const char *const g_nl_nl[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Ga naar debug-instellingen",
    /* English: ShadowMount+ v%s © Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s © Drakmor en de community",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 is verouderd, scan_depth=2 wordt gebruikt.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: scandiepte %u ingeschakeld.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-overlay mislukt: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport op game toegepast: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport op game toegepast: %s\nEmulators bijgewerkt",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Koppelen van image geweigerd:\n%s\nDe clustergrootte is te klein (%llu < %u).\nimage_sector-waarde opgeslagen: %u.\nProbeer opnieuw te koppelen.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Koppelen van image geweigerd:\n%s\nDe clustergrootte is te klein (%llu < %u).\nGebruik een groter cluster in de image of een kleinere sectorgrootte in de configuratie.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registreren mislukt: %s (%s)\nAppInstallTitleDir niet beschikbaar",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Registratie mislukt: %s (%s)\nTitleDir-bridge niet beschikbaar",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registreren mislukt: %s (%s)\ncode=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Installeren: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff weer actief",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff gepauzeerd terwijl de game draait",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Geïnstalleerd: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Onbekend",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Koppelen van image mislukt: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Clustergrootte van bestandssysteem (%llu) is kleiner dan apparaatsectorgrootte (%u) voor %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Gemounte image is onleesbaar of beschadigd",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Imagecache vol (%u vermeldingen), koppeling %s kan niet worden gevolgd",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Imagecache vol (%u vermeldingen), koppeling wordt ongedaan gemaakt",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Installatie mislukt: %s (%s)\nTime-out na 5 minuten",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Batchinstallatie in wachtrij (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... en nog %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Batchinstallatie mislukt voor %d apps.\ncode=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "%s is gecrasht voordat KStuff werd gepauzeerd. KStuff is niet de oorzaak.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s is gecrasht voordat KStuff werd gepauzeerd:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "%s is gecrasht nadat KStuff werd gepauzeerd. Automatische afstelling overgeslagen.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s is gecrasht nadat KStuff werd gepauzeerd:\n%s\nAutomatische afstelling overgeslagen.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Kan module niet laden na KStuff-pauze",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "kan module %.*s niet laden nadat KStuff is gepauzeerd",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Vertraging verhoogd naar %u s.\nStart de game opnieuw.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Vastloper gedetecteerd nadat KStuff werd gepauzeerd: de pauzevertraging voor %s is verhoogd naar %u s. Start de game opnieuw.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s is gecrasht nadat KStuff werd gepauzeerd:\n%s\nVertraging verhoogd naar %u s.\nStart de game opnieuw.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Vastloper gedetecteerd nadat KStuff werd gepauzeerd: %s. De vertraging voor automatische afstelling kon niet worden bijgewerkt.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s is gecrasht nadat KStuff werd gepauzeerd:\n%s\nKan de vertraging voor automatische afstelling niet bijwerken.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json ontbreekt of is ongeldig:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d nieuwe games gevonden. Verwerken...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliotheek gesynchroniseerd.\n%d games gevonden.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: configuratie opnieuw geladen.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplicaat %s genegeerd:\n%s\nbestaand: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR-emulator bijgewerkt",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: installeren van ShellCore-hooks mislukt",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Doeltemperatuur %u °C → %u °C",
};
