static const char *const g_nl_nl[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Ga naar debug-instellingen",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor en de community",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor en de community\nBeheer: http://%s:%u",
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
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Installatie mislukt: %s (%s)\nTime-out na 5 minuten",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Batchinstallatie in wachtrij (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... en nog %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Batchinstallatie mislukt voor %d apps.\ncode=0x%08X",
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
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Externe schijf %s aangesloten.\nVrij: %llu.%llu van %llu.%llu GB (blokgrootte %llu KB)\nGames op de schijf: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nGebruik voor maximale prestaties een clustergrootte van maximaal 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Externe schijf %s losgekoppeld",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Game %s is gecrasht.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Fout in game %s:\n%s",
};
