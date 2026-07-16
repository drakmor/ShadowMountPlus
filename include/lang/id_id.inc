static const char *const g_id_id[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Buka pengaturan debug",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 sudah usang, memakai scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: kedalaman pemindaian %u aktif.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport overlay gagal: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport game diterapkan: %s",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Pemasangan image ditolak:\n%s\nUkuran cluster terlalu kecil (%llu < %u).\nNilai image_sector disimpan: %u.\nCoba pasang lagi.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Pemasangan image ditolak:\n%s\nUkuran cluster terlalu kecil (%llu < %u).\nGunakan cluster yang lebih besar pada image atau ukuran sektor yang lebih kecil pada konfigurasi.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registrasi gagal: %s (%s)\nAppInstallTitleDir tidak tersedia",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Registrasi gagal: %s (%s)\nBridge TitleDir tidak tersedia",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registrasi gagal: %s (%s)\nkode=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Menginstal: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff aktif lagi",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff dijeda saat game berjalan",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Terinstal: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Tidak diketahui",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Pemasangan image gagal: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Ukuran cluster filesystem (%llu) lebih kecil dari ukuran sector perangkat (%u) untuk %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Image yang dimount tidak terbaca atau rusak",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Cache image penuh (%u entri), pemasangan %s tidak dapat dilacak",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Cache image penuh (%u entri), pemasangan dibatalkan",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Instalasi gagal: %s (%s)\nWaktu habis setelah 5 menit",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Instalasi batch masuk antrean (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... dan %d lagi",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Instalasi batch gagal untuk %d aplikasi.\nkode=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Aplikasi %s berhenti mendadak sebelum KStuff dijeda. KStuff bukan penyebabnya.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s berhenti mendadak sebelum KStuff dijeda:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Aplikasi %s berhenti mendadak setelah KStuff dijeda. Penyetelan otomatis dilewati.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s berhenti mendadak setelah KStuff dijeda:\n%s\nPenyetelan otomatis dilewati.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Tidak dapat memuat module setelah KStuff dijeda",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "tidak dapat memuat modul %.*s setelah KStuff dijeda",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Penundaan ditingkatkan menjadi %u dtk.\nJalankan game lagi.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Aplikasi berhenti mendadak setelah KStuff dijeda: penundaan untuk %s ditingkatkan menjadi %u dtk. Jalankan game lagi.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s berhenti mendadak setelah KStuff dijeda:\n%s\nPenundaan ditingkatkan menjadi %u dtk.\nJalankan game lagi.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Aplikasi berhenti mendadak setelah KStuff dijeda: %s. Gagal memperbarui penundaan penyetelan otomatis.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s berhenti mendadak setelah KStuff dijeda:\n%s\nGagal memperbarui penundaan penyetelan otomatis.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json hilang atau tidak valid:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Menemukan %d game baru. Memproses...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Pustaka disinkronkan.\nMenemukan %d game.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: konfigurasi dimuat ulang.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplikat %s diabaikan:\n%s\nyang ada: %s",
};
