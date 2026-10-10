static const char *const g_hu_hu[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Ugrás a hibakeresési beállításokhoz",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor és a közösség",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor és a közösség\nVezérlés: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: a recursive_scan=1 elavult, scan_depth=2 lesz használva.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: %u keresési mélység engedélyezve.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport overlay sikertelen: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Emulátorok alkalmazva a játékhoz: %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Játék backportolva: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Játék backportolva: %s\nEmulátorok frissítve",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "A lemezkép csatolása elutasítva:\n%s\nA klaszter mérete túl kicsi (%llu < %u).\nAz image_sector felülírása mentve: %u.\nPróbáld újra csatolni.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "A lemezkép csatolása elutasítva:\n%s\nA klaszter mérete túl kicsi (%llu < %u).\nHasználj nagyobb klasztert a lemezképben, vagy kisebb szektorméretet a beállításokban.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Regisztráció sikertelen: %s (%s)\nAppInstallTitleDir nem elérhető",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "A regisztráció sikertelen: %s (%s)\nA TitleDir-híd nem érhető el",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Regisztráció sikertelen: %s (%s)\nkód=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Telepítés: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Telepítve: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Ismeretlen",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "A lemezkép csatolása sikertelen: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "A fájlrendszer cluster mérete (%llu) kisebb, mint az eszköz szektormérete (%u): %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "A mountolt image olvashatatlan vagy sérült",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "A lemezkép-gyorsítótár megtelt (%u bejegyzés), a(z) %s csatolás nem követhető",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Telepítés sikertelen: %s (%s)\nIdőtúllépés 5 perc után",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Kötegelt telepítés sorban (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... és még %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "A kötegelt telepítés %d alkalmazásnál sikertelen.\nkód=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "Hiányzó vagy érvénytelen param.json:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d új játék található. Feldolgozás...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Könyvtár szinkronizálva.\n%d játék található.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: a beállítások újratöltve.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplikált %s kihagyva:\n%s\nlétező: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Az AMPR-emulátor frissítve",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: a ShellCore hookok telepítése sikertelen",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Célhőmérséklet %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Külső meghajtó csatlakoztatva: %s.\nSzabad: %llu.%llu / %llu.%llu GB (blokkméret %llu KB)\nJátékok a meghajtón: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nA maximális teljesítményhez használjon legfeljebb 64 KB-os klaszterméretet",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Külső meghajtó leválasztva: %s",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "A(z) %s játék összeomlott.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Hiba a(z) %s játékban:\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "Külső meghajtó csatlakoztatva: %s.",
};
