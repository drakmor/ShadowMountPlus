static const char *const g_pt_pt[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Ir para Definições de depuração",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor e a comunidade",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor e a comunidade\nControlo: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 está obsoleto, será usado scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: profundidade de pesquisa %u ativada.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Falha no overlay de backport: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Emuladores aplicados ao jogo: %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Jogo com backport aplicado: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Jogo com backport aplicado: %s\nEmuladores atualizados",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Montagem da imagem rejeitada:\n%s\nO tamanho do cluster é demasiado pequeno (%llu < %u).\nSubstituição image_sector guardada: %u.\nTente montar novamente.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Montagem da imagem rejeitada:\n%s\nO tamanho do cluster é demasiado pequeno (%llu < %u).\nUtilize um cluster maior na imagem ou um tamanho de setor menor na configuração.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registo falhou: %s (%s)\nAppInstallTitleDir indisponível",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Falha no registo: %s (%s)\nA ponte TitleDir não está disponível",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registo falhou: %s (%s)\ncódigo=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "A instalar: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Instalado: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Desconhecido",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Falha ao montar imagem: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "O tamanho do cluster do sistema de ficheiros (%llu) é menor que o tamanho do setor do dispositivo (%u) para %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "A imagem montada está ilegível ou danificada",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "A cache de imagens está cheia (%u entradas), não é possível acompanhar a montagem %s",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Instalação falhou: %s (%s)\nTempo esgotado após 5 minutos",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Instalação em lote na fila (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... e mais %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "A instalação em lote falhou para %d aplicações.\ncódigo=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json em falta ou inválido:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d novos jogos encontrados. A processar...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Biblioteca sincronizada.\n%d jogos encontrados.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: configuração recarregada.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplicado %s ignorado:\n%s\nexistente: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Emulador AMPR atualizado",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: não foi possível instalar os hooks do ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Temperatura-alvo %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Unidade externa %s ligada.\nLivre: %llu.%llu de %llu.%llu GB (bloco %llu KB)\nJogos na unidade: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nPara obter o máximo desempenho, utilize clusters de no máximo 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Unidade externa %s desligada",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "O jogo %s falhou.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Erro no jogo %s:\n%s",
};
