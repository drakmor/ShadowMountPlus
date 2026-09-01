static const char *const g_th_th[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "ไปที่การตั้งค่าดีบัก",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor และชุมชน",
    /* English: ShadowMount+ startup with web interface */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor และชุมชน\nควบคุม: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 เลิกใช้แล้ว จะใช้ scan_depth=2",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: เปิดใช้ความลึกการสแกน %u",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "backport overlay ล้มเหลว: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "ใช้ backport กับเกมแล้ว: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "ใช้ backport กับเกมแล้ว: %s\nอัปเดตอีมูเลเตอร์หลายตัวแล้ว",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "ปฏิเสธการเมานต์อิมเมจ:\n%s\nขนาดคลัสเตอร์เล็กเกินไป (%llu < %u)\nบันทึกค่า image_sector แล้ว: %u\nลองเมานต์อีกครั้ง",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "ปฏิเสธการเมานต์อิมเมจ:\n%s\nขนาดคลัสเตอร์เล็กเกินไป (%llu < %u)\nใช้คลัสเตอร์ที่ใหญ่ขึ้นในอิมเมจ หรือลดขนาดเซกเตอร์ในการตั้งค่า",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "ลงทะเบียนล้มเหลว: %s (%s)\nAppInstallTitleDir ไม่พร้อมใช้งาน",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "ลงทะเบียนไม่สำเร็จ: %s (%s)\nบริดจ์ TitleDir ไม่พร้อมใช้งาน",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "ลงทะเบียนล้มเหลว: %s (%s)\nรหัส=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "กำลังติดตั้ง: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff ทำงานอีกครั้ง",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "พัก KStuff ขณะเกมกำลังทำงาน",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "ติดตั้งแล้ว: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "ไม่ทราบ",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "เมานต์อิมเมจไม่สำเร็จ: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "ขนาด cluster ของ filesystem (%llu) เล็กกว่าขนาด sector ของอุปกรณ์ (%u) สำหรับ %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "อิมเมจที่เมานต์ไว้อ่านไม่ได้หรือเสียหาย",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "แคชอิมเมจเต็ม (%u รายการ) จึงติดตามการเมานต์ %s ไม่ได้",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "แคชอิมเมจเต็ม (%u รายการ) กำลังย้อนกลับการเมานต์",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "ติดตั้งล้มเหลว: %s (%s)\nหมดเวลาหลัง 5 นาที",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "เพิ่มการติดตั้งแบบชุดเข้าคิวแล้ว (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... และอีก %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "การติดตั้งแบบชุดล้มเหลวสำหรับ %d แอป\nรหัส=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "แอป %s ขัดข้องก่อนพัก KStuff โดย KStuff ไม่ใช่สาเหตุ",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s ขัดข้องก่อนหยุด KStuff ชั่วคราว:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "แอป %s ขัดข้องหลังพัก KStuff จึงข้ามการปรับอัตโนมัติ",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s ขัดข้องหลังหยุด KStuff ชั่วคราว:\n%s\nข้ามการปรับอัตโนมัติ",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "โหลดโมดูลหลังพัก KStuff ไม่ได้",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "ไม่สามารถโหลดโมดูล %.*s หลังหยุด KStuff ชั่วคราว",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. เพิ่ม delay เป็น %u วินาที\nเปิดเกมอีกครั้ง",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "พบการขัดข้องหลังพัก KStuff: เพิ่มเวลาหน่วงของ %s เป็น %u วินาที เปิดเกมอีกครั้ง",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s ขัดข้องหลังหยุด KStuff ชั่วคราว:\n%s\nเพิ่มเวลาหน่วงเป็น %u วินาที\nเปิดเกมอีกครั้ง",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "พบการขัดข้องหลังพัก KStuff: %s ไม่สามารถอัปเดตเวลาหน่วงการปรับอัตโนมัติ",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s ขัดข้องหลังหยุด KStuff ชั่วคราว:\n%s\nไม่สามารถอัปเดตเวลาหน่วงการปรับอัตโนมัติ",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json หายไปหรือไม่ถูกต้อง:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "พบเกมใหม่ %d เกม กำลังประมวลผล...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "ซิงค์ไลบรารีแล้ว\nพบ %d เกม",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: โหลดการตั้งค่าใหม่แล้ว",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "ข้ามรายการซ้ำ %s:\n%s\nที่มีอยู่: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "อัปเดตโปรแกรมจำลอง AMPR แล้ว",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: ไม่สามารถติดตั้งฮุก ShellCore ได้",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "อุณหภูมิเป้าหมาย %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d */
    [SM_L10N_USB_CONNECTED_SCANNING] = "เชื่อมต่อไดรฟ์ภายนอก %s แล้ว\nว่าง: %llu.%llu จาก %llu.%llu GB (ขนาดบล็อก %llu KB)\nเกมในไดรฟ์: %d%s",
    /* English: For maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nเพื่อประสิทธิภาพสูงสุด ให้ใช้ขนาดคลัสเตอร์ไม่เกิน 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "ตัดการเชื่อมต่อไดรฟ์ภายนอก %s แล้ว",
};
