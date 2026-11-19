// tweak_main.c — dylib 入口：加载配置 → 等游戏镜像 → 应用补丁 → 启动锁定线程
#include "tweak_main.h"
#include "pvz_tweak.h"
#include "mach_compat.h"

__attribute__((constructor))
static void tweak_entry(void) {
    TweakConfig cfg;
    config_load(&cfg);

    TWEAK_LOG("init: image=%s poll=%dms sun=%d(%ld) coins=%d cooldown=%d",
              cfg.image_keyword, cfg.poll_ms,
              cfg.sun.enabled, cfg.sun.value,
              cfg.coins.enabled, cfg.cooldown.enabled);

    pthread_t th;
    TweakConfig *heap = malloc(sizeof(cfg));
    *heap = cfg;
    if (pthread_create(&th, NULL, tweak_worker, heap) == 0)
        pthread_detach(th);
}

void *tweak_worker(void *arg) {
    TweakConfig *cfg = arg;

    // 等待游戏主镜像加载完成（注入发生在启动早期）
    for (int i = 0; i < 600 && !mem_image_base(cfg->image_keyword); i++) usleep(100000);
    if (!mem_image_base(cfg->image_keyword)) {
        TWEAK_LOG("game image not found, exit worker");
        free(cfg);
        return NULL;
    }
    TWEAK_LOG("game image ready");

    // 一次性补丁类功能
    mod_cooldown_apply(&cfg->cooldown, cfg->image_keyword);

    // 周期性数值锁定
    for (;;) {
        mod_sun_tick(&cfg->sun, cfg->image_keyword);
        mod_coins_tick(&cfg->coins, cfg->image_keyword);
        usleep(cfg->poll_ms > 0 ? cfg->poll_ms * 1000 : 100000);
    }
}
