// menu.m — 游戏内悬浮菜单(高数带我飞式)
// 悬浮球(可拖动)→ ActionSheet:阳光/金币锁定、全卡池、任意放置、
// 模式跳转(我是僵尸/冒险/挑战/塔/宝藏)、存档落盘
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <unistd.h>
#import <objc/runtime.h>
#include "tweak_ctl.h"

static UIWindow *g_window = nil;
static UIButton *g_ball = nil;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@interface PvZBallVC : UIViewController
@end
@implementation PvZBallVC
@end

@interface PvZBallButton : UIButton
@property (copy) void (^onTap)(void);
@property (copy) void (^onPan)(CGPoint delta);
@end
@implementation PvZBallButton
- (void)internalTap {
    if (self.onTap) self.onTap();
}
- (void)internalPan:(UIPanGestureRecognizer *)g {
    if (!self.onPan) return;
    CGPoint d = [g translationInView:self.superview];
    if (g.state == UIGestureRecognizerStateChanged)
        self.onPan(d);
    if (g.state == UIGestureRecognizerStateEnded)
        [g setTranslation:CGPointZero inView:self.superview];
}
@end

static void refresh_menu(void) { /* 预留:动态刷新球上文字 */ }

static void show_menu(void) {
    if (!g_window || !g_window.rootViewController) return;
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"PvZ 融合修改器 v0.5"
                                 message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    NSString *sun = ctl_sun_enabled() ? @"☀️ 阳光锁定:开(点击关闭)" : @"☀️ 阳光锁定:关(点击开启)";
    NSString *money = ctl_money_enabled() ? @"💰 金币锁定:开(点击关闭)" : @"💰 金币锁定:关(点击开启)";
    NSString *place = ctl_place_enabled() ? @"🪴 任意放置:开(点击关闭)" : @"🪴 任意放置:豌豆上鼠标";

    void (^mk)(NSString *, void (^)(void)) = ^(NSString *title, void (^handler)(void)) {
        [sheet addAction:[UIAlertAction actionWithTitle:title
            style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) { handler(); }]];
    };

    mk(sun, ^{ ctl_sun_toggle(); show_menu(); });
    mk(money, ^{ ctl_money_toggle(); show_menu(); });
    mk(@"🌱 全卡池(任意选卡)", ^{ ctl_seed_all_fire(); });
    mk(place, ^{ ctl_place_toggle(-1); });
    mk(@"🪴 放置:豌豆射手", ^{ ctl_place_toggle(0); });
    mk(@"🔥 放置:火炬树桩", ^{ ctl_place_toggle(21); });
    mk(@"💾 立即保存存档", ^{ ctl_save_now(); });
    mk(@"🧟 跳转:我是僵尸 第1关", ^{ ctl_jump("IZ", 1); });
    mk(@"🧟 跳转:我是僵尸 第2关", ^{ ctl_jump("IZ", 2); });
    mk(@"🌻 跳转:冒险模式 第1关", ^{ ctl_jump("adv", 1); });
    mk(@"🏛 跳转:主菜单", ^{ ctl_jump("menu", 0); });
    mk(@"💎 跳转:宝藏菜单", ^{ ctl_jump("treasure", 0); });
    mk(@"🗼 跳转:塔防菜单", ^{ ctl_jump("tower", 0); });
    mk(@"🏆 跳转:挑战菜单", ^{ ctl_jump("challenge", 0); });
    mk(@"取消", nil);

    UIViewController *vc = g_window.rootViewController;
    if (sheet.popoverPresentationController)
        sheet.popoverPresentationController.sourceView = vc.view;
    [vc presentViewController:sheet animated:YES completion:nil];
}

static void setup_ball(void) {
    if (g_window) return;
    if (![UIApplication sharedApplication]) return;

    PvZBallButton *btn = [PvZBallButton buttonWithType:UIButtonTypeCustom];
    CGFloat size = 56;
    btn.frame = CGRectMake(12, 180, size, size);
    btn.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    btn.layer.cornerRadius = size / 2;
    btn.layer.borderWidth = 1;
    btn.layer.borderColor = [UIColor whiteColor].CGColor;
    [btn setTitle:@"PvZ" forState:UIControlStateNormal];
    [btn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont boldSystemFontOfSize:15];

    PvZBallButton __unsafe_unretained *wbtn = btn;
    btn.onTap = ^{ show_menu(); };
    btn.onPan = ^(CGPoint d) {
        CGRect f = wbtn.frame;
        f.origin.x += d.x; f.origin.y += d.y;
        // 限制在屏幕内
        CGSize ss = wbtn.superview.bounds.size;
        f.origin.x = MAX(0, MIN(f.origin.x, ss.width - f.size.width));
        f.origin.y = MAX(0, MIN(f.origin.y, ss.height - f.size.height));
        wbtn.frame = f;
    };
    [btn addTarget:btn action:@selector(internalTap) forControlEvents:UIControlEventTouchUpInside];
    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc]
        initWithTarget:btn action:@selector(internalPan:)];
    [btn addGestureRecognizer:pan];

    PvZBallVC *vc = [[PvZBallVC alloc] init];
    vc.view.backgroundColor = [UIColor clearColor];
    g_window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    g_window.windowLevel = UIWindowLevelAlert + 100;
    g_window.rootViewController = vc;
    g_window.hidden = NO;
    [vc.view addSubview:btn];
    g_ball = btn;
    NSLog(@"[PvZTweak] menu ball ready");
}

#pragma clang diagnostic pop

// ---- 菜单线程:延迟等 UIApplication 就绪(Unity 初始化 ~5-20s) ----
static void *menu_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 600; i++) { // 最多 120s
        if ([UIApplication sharedApplication]) break;
        usleep(200000);
    }
    // 再等 3 秒让 Unity window 稳定
    usleep(3000000);
    dispatch_async(dispatch_get_main_queue(), ^{ setup_ball(); });
    return NULL;
}

__attribute__((constructor))
static void menu_bootstrap(void) {
    pthread_t th;
    pthread_create(&th, NULL, menu_thread, NULL);
    pthread_detach(th);
}
