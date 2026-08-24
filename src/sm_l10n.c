#include "sm_l10n.h"

#include "sm_config_mount.h"
#include "sm_log.h"
#include "sm_types.h"

#include <stdatomic.h>

#define SCE_SYSTEM_SERVICE_PARAM_ID_LANG 1
#define SM_LANGUAGE_EN_US 1
#define SM_SYSTEM_LANGUAGE_COUNT 31
#define SM_LANGUAGE_UNINITIALIZED (-2)

#include "lang/ar_sa.inc"
#include "lang/cs_cz.inc"
#include "lang/da_dk.inc"
#include "lang/de_de.inc"
#include "lang/el_gr.inc"
#include "lang/en_gb.inc"
#include "lang/en_us.inc"
#include "lang/es_es.inc"
#include "lang/es_mx.inc"
#include "lang/fi_fi.inc"
#include "lang/fr_ca.inc"
#include "lang/fr_fr.inc"
#include "lang/hu_hu.inc"
#include "lang/id_id.inc"
#include "lang/it_it.inc"
#include "lang/ja_jp.inc"
#include "lang/ko_kr.inc"
#include "lang/nl_nl.inc"
#include "lang/no_no.inc"
#include "lang/pl_pl.inc"
#include "lang/pt_br.inc"
#include "lang/pt_pt.inc"
#include "lang/ro_ro.inc"
#include "lang/ru_ru.inc"
#include "lang/sv_se.inc"
#include "lang/th_th.inc"
#include "lang/tr_tr.inc"
#include "lang/uk_ua.inc"
#include "lang/vi_vn.inc"
#include "lang/zh_cn.inc"
#include "lang/zh_tw.inc"

typedef struct {
  const char *country;
  const char *lang;
  const char *locale;
  const char *const *catalog;
} sm_region_t;

static const sm_region_t g_regions[] = {
    {"jp", "ja", "ja-JP", g_ja_jp}, {"us", "en", "en-US", g_en_us},
    {"fr", "fr", "fr-FR", g_fr_fr}, {"es", "es", "es-ES", g_es_es},
    {"de", "de", "de-DE", g_de_de}, {"it", "it", "it-IT", g_it_it},
    {"nl", "nl", "nl-NL", g_nl_nl}, {"pt", "pt", "pt-PT", g_pt_pt},
    {"ru", "ru", "ru-RU", g_ru_ru}, {"kr", "ko", "ko-KR", g_ko_kr},
    {"tw", "zh", "zh-TW", g_zh_tw}, {"cn", "zh", "zh-CN", g_zh_cn},
    {"fi", "fi", "fi-FI", g_fi_fi}, {"se", "sv", "sv-SE", g_sv_se},
    {"dk", "da", "da-DK", g_da_dk}, {"no", "no", "no-NO", g_no_no},
    {"pl", "pl", "pl-PL", g_pl_pl}, {"br", "pt", "pt-BR", g_pt_br},
    {"gb", "en", "en-GB", g_en_gb}, {"tr", "tr", "tr-TR", g_tr_tr},
    {"mx", "es", "es-MX", g_es_mx}, {"sa", "ar", "ar-SA", g_ar_sa},
    {"ca", "fr", "fr-CA", g_fr_ca}, {"cz", "cs", "cs-CZ", g_cs_cz},
    {"hu", "hu", "hu-HU", g_hu_hu}, {"gr", "el", "el-GR", g_el_gr},
    {"ro", "ro", "ro-RO", g_ro_ro}, {"th", "th", "th-TH", g_th_th},
    {"vn", "vi", "vi-VN", g_vi_vn}, {"id", "id", "id-ID", g_id_id},
    {"ua", "uk", "uk-UA", g_uk_ua},
};

#define SM_REGION_COUNT (sizeof(g_regions) / sizeof(g_regions[0]))

_Static_assert(SM_REGION_COUNT == SM_SYSTEM_LANGUAGE_COUNT,
               "unexpected language count");

static atomic_int g_active_lang = ATOMIC_VAR_INIT(SM_LANGUAGE_UNINITIALIZED);

static bool is_valid_language_id(int32_t language_id) {
  return language_id >= 0 && language_id < (int32_t)SM_REGION_COUNT;
}

static bool is_valid_system_language_id(int32_t language_id) {
  return language_id >= 0 && language_id < SM_SYSTEM_LANGUAGE_COUNT;
}

static bool language_code_matches(const char *value, const char *code) {
  char normalized[16];
  size_t len = strlen(code);
  if (len >= sizeof(normalized))
    return false;

  for (size_t i = 0; i < len; ++i) {
    char ch = code[i];
    normalized[i] = (ch == '-') ? '_' : ch;
  }
  normalized[len] = '\0';

  return strcasecmp(value, code) == 0 || strcasecmp(value, normalized) == 0;
}

bool sm_l10n_parse_language_id(const char *value, int32_t *language_id_out) {
  if (!value || !language_id_out)
    return false;

  if (strcasecmp(value, "auto") == 0 || strcasecmp(value, "system") == 0 ||
      strcasecmp(value, "default") == 0) {
    *language_id_out = SM_LANGUAGE_AUTO;
    return true;
  }

  for (size_t i = 0; i < SM_REGION_COUNT; ++i) {
    const sm_region_t *region = &g_regions[i];
    if (language_code_matches(value, region->locale) ||
        strcasecmp(value, region->country) == 0 ||
        strcasecmp(value, region->lang) == 0) {
      *language_id_out = (int32_t)i;
      return true;
    }
  }

  return false;
}

const char *sm_l10n_language_name(int32_t language_id) {
  if (language_id == SM_LANGUAGE_AUTO)
    return "auto";
  if (!is_valid_language_id(language_id))
    return "invalid";
  return g_regions[language_id].locale;
}

void sm_l10n_init(void) {
  int32_t sys_lang = -1;
  const runtime_config_t *cfg = runtime_config();
  int32_t active_lang = cfg->language_id;
  bool auto_language = (active_lang == SM_LANGUAGE_AUTO);

  if (auto_language &&
      sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG,
                                  &sys_lang) == 0 &&
      is_valid_system_language_id(sys_lang)) {
    active_lang = sys_lang;
  } else if (auto_language) {
    active_lang = SM_LANGUAGE_EN_US;
  }

  if (!is_valid_language_id(active_lang))
    active_lang = SM_LANGUAGE_EN_US;

  int previous_lang = atomic_load_explicit(&g_active_lang, memory_order_relaxed);
  atomic_store_explicit(&g_active_lang, active_lang, memory_order_release);

  if (previous_lang != active_lang) {
    const sm_region_t *region = &g_regions[active_lang];
    if (auto_language) {
      log_debug("  [L10N] language=auto active=%d country=%s lang=%s locale=%s",
                active_lang, region->country, region->lang, region->locale);
    } else {
      log_debug("  [L10N] language=config active=%d country=%s lang=%s locale=%s",
                active_lang, region->country, region->lang, region->locale);
    }
  }
}

const char *sm_l10n_get(sm_l10n_key_t key) {
  if (key < 0 || key >= SM_L10N_COUNT)
    return "";

  int active_lang = atomic_load_explicit(&g_active_lang, memory_order_acquire);
  if (!is_valid_language_id(active_lang)) {
    sm_l10n_init();
    active_lang = atomic_load_explicit(&g_active_lang, memory_order_acquire);
  }

  const char *const *catalog = g_regions[active_lang].catalog;
  if (catalog[key])
    return catalog[key];
  return g_en_us[key] ? g_en_us[key] : "";
}
