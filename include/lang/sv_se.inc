static const char *const g_sv_se[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Gå till felsökningsinställningar",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor och gemenskapen",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor och gemenskapen\nStyrning: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 är föråldrat, använder scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: skanningsdjup %u aktiverat.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-overlay misslyckades: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Emulatorer tillämpade på spelet: %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport tillämpad på spelet: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport tillämpad på spelet: %s\nEmulatorer uppdaterade",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Image-montering nekades:\n%s\nKlusterstorleken är för liten (%llu < %u).\nimage_sector-åsidosättning sparad: %u.\nFörsök montera igen.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Montering av image nekades:\n%s\nKlusterstorleken är för liten (%llu < %u).\nAnvänd en större klusterstorlek i imagen eller en mindre sektorstorlek i konfigurationen.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registrering misslyckades: %s (%s)\nAppInstallTitleDir inte tillgänglig",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Registreringen misslyckades: %s (%s)\nTitleDir-bryggan är inte tillgänglig",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registrering misslyckades: %s (%s)\nkod=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Installerar: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Installerat: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Okänd",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Image-montering misslyckades: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Filsystemets klusterstorlek (%llu) är mindre än enhetens sektorstorlek (%u) för %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Monterad image är oläsbar eller skadad",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Image-cachen är full (%u poster), monteringen %s kan inte spåras",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Installation misslyckades: %s (%s)\nTidsgräns efter 5 minuter",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Batchinstallation köad (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... och %d till",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Batchinstallationen misslyckades för %d appar.\nkod=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json saknas eller är ogiltig:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Hittade %d nya spel. Bearbetar...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliotek synkroniserat.\nHittade %d spel.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: konfigurationen lästes in på nytt.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Dubblett %s ignorerad:\n%s\nbefintlig: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR-emulatorn har uppdaterats",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: det gick inte att installera ShellCore-hooks",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Måltemperatur %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Extern enhet %s ansluten.\nLedigt: %llu.%llu av %llu.%llu GB (blockstorlek %llu KB)\nSpel på enheten: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nAnvänd en klusterstorlek på högst 64 KB för maximal prestanda",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Extern enhet %s frånkopplad",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Spelet %s kraschade.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Fel i spelet %s:\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "Extern enhet %s ansluten.",
};
