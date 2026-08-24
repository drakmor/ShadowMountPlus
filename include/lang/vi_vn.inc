static const char *const g_vi_vn[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Đi tới cài đặt gỡ lỗi",
    /* English: ShadowMount+ v%s © Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s © Drakmor và Cộng đồng",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 đã lỗi thời; sẽ sử dụng scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: đã bật độ sâu quét %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport overlay thất bại: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Đã áp dụng backport cho game: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Đã áp dụng backport cho game: %s\nĐã cập nhật các trình giả lập",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Từ chối gắn ảnh đĩa:\n%s\nKích thước cụm quá nhỏ (%llu < %u).\nĐã lưu giá trị image_sector: %u.\nHãy thử gắn lại.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Từ chối gắn ảnh đĩa:\n%s\nKích thước cụm quá nhỏ (%llu < %u).\nHãy dùng cụm lớn hơn trong ảnh đĩa hoặc kích thước cung từ nhỏ hơn trong cấu hình.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Đăng ký thất bại: %s (%s)\nAppInstallTitleDir không khả dụng",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Đăng ký thất bại: %s (%s)\nCầu nối TitleDir không khả dụng",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Đăng ký thất bại: %s (%s)\nmã=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Đang cài đặt: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff hoạt động lại",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff tạm dừng khi game đang chạy",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Đã cài đặt: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Không xác định",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Không thể gắn ảnh đĩa: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Kích thước cluster của hệ thống tệp (%llu) nhỏ hơn kích thước sector thiết bị (%u) cho %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Ảnh đĩa đã gắn không thể đọc được hoặc bị hỏng",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Bộ nhớ đệm ảnh đĩa đã đầy (%u mục), không thể theo dõi điểm gắn %s",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Bộ nhớ đệm ảnh đĩa đã đầy (%u mục), đang hoàn tác việc gắn",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Cài đặt thất bại: %s (%s)\nHết thời gian sau 5 phút",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Cài đặt hàng loạt đã vào hàng đợi (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... và thêm %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Cài đặt hàng loạt thất bại cho %d ứng dụng.\nmã=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Ứng dụng %s gặp sự cố trước khi KStuff tạm dừng. KStuff không phải là nguyên nhân.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s gặp sự cố trước khi tạm dừng KStuff:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Ứng dụng %s gặp sự cố sau khi KStuff tạm dừng. Đã bỏ qua tinh chỉnh tự động.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s gặp sự cố sau khi tạm dừng KStuff:\n%s\nĐã bỏ qua tự động tinh chỉnh.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Không thể tải module sau khi KStuff tạm dừng",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "không thể tải mô-đun %.*s sau khi tạm dừng KStuff",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Độ trễ đã tăng lên %u giây.\nHãy khởi chạy lại trò chơi.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Phát hiện sự cố sau khi KStuff tạm dừng: độ trễ của %s đã tăng lên %u giây. Hãy khởi chạy lại trò chơi.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s gặp sự cố sau khi tạm dừng KStuff:\n%s\nĐộ trễ đã tăng lên %u giây.\nHãy khởi chạy lại trò chơi.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Phát hiện sự cố sau khi KStuff tạm dừng: %s. Không thể cập nhật độ trễ tinh chỉnh tự động.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s gặp sự cố sau khi tạm dừng KStuff:\n%s\nKhông thể cập nhật độ trễ tự động tinh chỉnh.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json thiếu hoặc không hợp lệ:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Tìm thấy %d trò chơi mới. Đang xử lý...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Đã đồng bộ thư viện.\nTìm thấy %d trò chơi.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: đã tải lại cấu hình.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Đã bỏ qua bản trùng %s:\n%s\nđã có: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Đã cập nhật trình giả lập AMPR",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: không thể cài đặt hook ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Nhiệt độ mục tiêu %u °C → %u °C",
};
