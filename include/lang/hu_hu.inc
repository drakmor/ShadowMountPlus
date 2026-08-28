static const char *const g_hu_hu[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Ugrás a hibakeresési beállításokhoz",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor és a közösség",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: a recursive_scan=1 elavult, scan_depth=2 lesz használva.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: %u keresési mélység engedélyezve.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport overlay sikertelen: %s\n%s\n0x%08X",
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
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "A KStuff újra aktív",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "A KStuff szünetel, amíg a játék fut",
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
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "A lemezkép-gyorsítótár megtelt (%u bejegyzés), a csatolás visszavonása folyamatban",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Telepítés sikertelen: %s (%s)\nIdőtúllépés 5 perc után",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Kötegelt telepítés sorban (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... és még %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "A kötegelt telepítés %d alkalmazásnál sikertelen.\nkód=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "A(z) %s összeomlott a KStuff szüneteltetése előtt. Nem a KStuff a hibás.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "A(z) %s összeomlott a KStuff szüneteltetése előtt:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "A(z) %s összeomlott a KStuff szüneteltetése után. Az automatikus hangolás kimaradt.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "A(z) %s összeomlott a KStuff szüneteltetése után:\n%s\nAz automatikus hangolás kihagyva.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "A modul nem tölthető be a KStuff szünet után",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "a(z) %.*s modul nem tölthető be a KStuff szüneteltetése után",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Késleltetés %u s értékre növelve.\nIndítsd újra a játékot.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Összeomlás történt a KStuff szüneteltetése után: a(z) %s késleltetése %u másodpercre nőtt. Indítsd újra a játékot.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "A(z) %s összeomlott a KStuff szüneteltetése után:\n%s\nA késleltetés %u másodpercre nőtt.\nIndítsd újra a játékot.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Összeomlás történt a KStuff szüneteltetése után: %s. Az automatikus hangolás késleltetése nem frissíthető.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "A(z) %s összeomlott a KStuff szüneteltetése után:\n%s\nAz automatikus hangolás késleltetése nem frissíthető.",
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
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Külső meghajtó csatlakoztatva: %s.\nSzabad: %llu.%llu / %llu.%llu GB (blokkméret %llu KB)\nJátékok a meghajtón: %d%s",
    /* English: For maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nA maximális teljesítményhez használjon legfeljebb 64 KB-os klaszterméretet",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Külső meghajtó leválasztva: %s",
};
