static const char *const g_no_no[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Gå til feilsøkingsinnstillinger",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor og fellesskapet",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor og fellesskapet\nKontroll: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 er utdatert, bruker scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: skannedybde %u aktivert.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport-overlay mislyktes: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport brukt på spillet: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport brukt på spillet: %s\nEmulatorer oppdatert",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Montering av image avvist:\n%s\nKlyngestørrelsen er for liten (%llu < %u).\nimage_sector-overstyring lagret: %u.\nPrøv å montere igjen.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Montering av image avvist:\n%s\nKlyngestørrelsen er for liten (%llu < %u).\nBruk en større klyngestørrelse i imaget eller en mindre sektorstørrelse i konfigurasjonen.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registrering mislyktes: %s (%s)\nAppInstallTitleDir utilgjengelig",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Registrering mislyktes: %s (%s)\nTitleDir-broen er ikke tilgjengelig",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registrering mislyktes: %s (%s)\nkode=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Installerer: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Installert: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Ukjent",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Montering av image mislyktes: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Filsystemets klyngestørrelse (%llu) er mindre enn enhetens sektorstørrelse (%u) for %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Mountet image er uleselig eller skadet",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Image-cachen er full (%u oppføringer), monteringen %s kan ikke spores",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Installasjon mislyktes: %s (%s)\nTidsavbrudd etter 5 minutter",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Batchinstallasjon i kø (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... og %d til",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Batchinstallasjonen mislyktes for %d apper.\nkode=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json mangler eller er ugyldig:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Fant %d nye spill. Behandler...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliotek synkronisert.\nFant %d spill.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: konfigurasjonen ble lastet inn på nytt.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplikat %s ignorert:\n%s\neksisterende: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR-emulator oppdatert",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: kunne ikke installere ShellCore-hooks",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Måltemperatur %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Ekstern disk %s tilkoblet.\nLedig: %llu.%llu av %llu.%llu GB (blokkstørrelse %llu KB)\nSpill på disken: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nBruk en klyngestørrelse på maksimalt 64 KB for maksimal ytelse",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Ekstern disk %s frakoblet",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Spillet %s krasjet.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Feil i spillet %s:\n%s",
};
