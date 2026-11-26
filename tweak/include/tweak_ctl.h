// tweak_ctl.h — 菜单 UI → 修改器核心 的控制接口(menu.m 调用)
#ifndef TWEAK_CTL_H
#define TWEAK_CTL_H

// 状态查询(菜单显示用,返回 0/1)
int ctl_sun_enabled(void);
int ctl_money_enabled(void);
int ctl_seed_all_pending(void);
int ctl_place_enabled(void);

// 开关
void ctl_sun_toggle(void);        // 阳光锁定 on/off
void ctl_money_toggle(void);      // 金币锁定 on/off
void ctl_seed_all_fire(void);     // 立即全卡池(board 就绪后生效)
void ctl_place_toggle(int plant); // 任意放置 on/off(plant<0 只切换不改类型)
void ctl_save_now(void);          // 立即存档落盘

// 模式跳转(mode: "IZ"/"adv"/"menu"/"treasure"/"tower"/"challenge"/"izmenu")
void ctl_jump(const char *mode, int level);

#endif
