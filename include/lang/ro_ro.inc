static const char *const g_ro_ro[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Mergi la setările de depanare",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor și comunitatea",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor și comunitatea\nControl: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 este depășit, se folosește scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: adâncimea de scanare %u este activată.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Overlay backport eșuat: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Emulatoare aplicate jocului: %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport aplicat jocului: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport aplicat jocului: %s\nEmulatoare actualizate",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Montarea imaginii respinsă:\n%s\nDimensiunea clusterului este prea mică (%llu < %u).\nOverride image_sector salvat: %u.\nÎncercați montarea din nou.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Montarea imaginii a fost respinsă:\n%s\nDimensiunea clusterului este prea mică (%llu < %u).\nFolosiți un cluster mai mare în imagine sau o dimensiune mai mică a sectorului în configurație.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Înregistrare eșuată: %s (%s)\nAppInstallTitleDir indisponibil",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Înregistrarea a eșuat: %s (%s)\nPuntea TitleDir nu este disponibilă",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Înregistrare eșuată: %s (%s)\ncod=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Se instalează: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Instalat: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Necunoscut",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Montarea imaginii a eșuat: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Dimensiunea clusterului sistemului de fișiere (%llu) este mai mică decât sectorul dispozitivului (%u) pentru %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Imaginea montată este ilizibilă sau deteriorată",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Cache-ul de imagini este plin (%u intrări), montarea %s nu poate fi urmărită",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Instalare eșuată: %s (%s)\nTimeout după 5 minute",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Instalare în lot pusă în coadă (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... și încă %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Instalarea în lot a eșuat pentru %d aplicații.\ncod=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json lipsește sau este invalid:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "S-au găsit %d jocuri noi. Se procesează...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliotecă sincronizată.\nS-au găsit %d jocuri.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: configurația a fost reîncărcată.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplicatul %s ignorat:\n%s\nexistent: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Emulatorul AMPR a fost actualizat",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: instalarea hook-urilor ShellCore a eșuat",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Temperatura țintă %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Unitatea externă %s a fost conectată.\nLiber: %llu.%llu din %llu.%llu GB (dimensiunea blocului %llu KB)\nJocuri pe unitate: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nPentru performanță maximă, utilizați o dimensiune a clusterului de cel mult 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Unitatea externă %s a fost deconectată",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Jocul %s s-a blocat.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Eroare în jocul %s:\n%s",
};
