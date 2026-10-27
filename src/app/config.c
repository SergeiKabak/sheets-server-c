#include "app/config.h"

#include <stdio.h>
#include <stdlib.h>

void config_load(AppConfig *cfg) {
    if (!cfg) return;
    cfg->bot_token      = getenv("BOT_TOKEN");
    cfg->spreadsheet_id = getenv("SPREADSHEET_ID");
    cfg->google_api_key = getenv("GOOGLE_API_KEY");
    cfg->from_col       = getenv("FROM_COL");
    cfg->to_col         = getenv("TO_COL");
    cfg->search_name    = getenv("SEARCH_NAME");
    cfg->miniapp_url    = getenv("MINIAPP_URL");
}

int config_validate(const AppConfig *cfg) {
    if (!cfg) return -1;
    int ok = 1;
    if (!cfg->bot_token)      { fprintf(stderr, "  - BOT_TOKEN\n"); ok = 0; }
    if (!cfg->spreadsheet_id) { fprintf(stderr, "  - SPREADSHEET_ID\n"); ok = 0; }
    if (!cfg->google_api_key) { fprintf(stderr, "  - GOOGLE_API_KEY\n"); ok = 0; }
    if (!cfg->search_name)    { fprintf(stderr, "  - SEARCH_NAME\n"); ok = 0; }
    return ok ? 0 : -1;
}
