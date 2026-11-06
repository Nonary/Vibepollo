/**
 * @file src/tray_darwin.m
 * @brief System tray implementation for macOS.
 */
// standard includes
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// lib includes
#include <Cocoa/Cocoa.h>

// local includes
#include "third-party/tray/src/tray.h"

/**
 * @class AppDelegate
 * @brief The application delegate that handles menu actions.
 */
@interface AppDelegate: NSObject <NSApplicationDelegate>
/**
 * @brief Callback function for menu item actions.
 * @param sender The object that sent the action message.
 * @return void
 */
- (IBAction)menuCallback:(id)sender;
@end

@implementation AppDelegate {
}

- (IBAction)menuCallback:(id)sender {
  struct tray_menu *m = [[sender representedObject] pointerValue];
  if (m != NULL && m->cb != NULL) {
    m->cb(m);
  }
}

@end

static NSApplication *app;
static NSStatusBar *statusBar;
static NSStatusItem *statusItem;
static AppDelegate *appDelegate;
static int g_tray_exited = 0;

static tray_log_callback g_tray_log_cb = NULL;

void tray_set_log_callback(tray_log_callback cb) {
  g_tray_log_cb = cb;
}

static void tray_log(enum tray_log_level level, const char *fmt, ...) {
  if (!g_tray_log_cb || !fmt) {
    return;
  }
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  buffer[sizeof(buffer) - 1] = '\0';
  g_tray_log_cb(level, buffer);
}

static NSMenu *_tray_menu(struct tray_menu *m) {
  NSMenu *menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems:FALSE];

  for (; m != NULL && m->text != NULL; m++) {
    if (strcmp(m->text, "-") == 0) {
      [menu addItem:[NSMenuItem separatorItem]];
    } else {
      NSMenuItem *menuItem = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithUTF8String:m->text]
               action:@selector(menuCallback:)
        keyEquivalent:@""];
      [menuItem setTarget:appDelegate];
      [menuItem setEnabled:(m->disabled ? FALSE : TRUE)];
      [menuItem setState:(m->checked ? 1 : 0)];
      [menuItem setRepresentedObject:[NSValue valueWithPointer:m]];
      [menu addItem:menuItem];
      if (m->submenu != NULL) {
        [menu setSubmenu:_tray_menu(m->submenu) forItem:menuItem];
      }
    }
  }
  return menu;
}

int tray_init(struct tray *tray) {
  if (![NSThread isMainThread]) {
    tray_log(TRAY_LOG_ERROR, "tray_init must be called on the main thread");
    return -1;
  }
  app = [NSApplication sharedApplication];
  [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

  if (!appDelegate) {
    appDelegate = [[AppDelegate alloc] init];
  }
  [app setDelegate:appDelegate];

  statusBar = [NSStatusBar systemStatusBar];
  if (statusItem == nil) {
    statusItem = [statusBar statusItemWithLength:NSVariableStatusItemLength];
    statusItem.autosaveName = @"io.github.Nonary.vibepollo.tray";
  }
  if (statusBar == nil || statusItem == nil) {
    tray_log(TRAY_LOG_ERROR, "Failed to initialize NSStatusBar/NSStatusItem");
    return -1;
  }

  g_tray_exited = 0;
  tray_update(tray);
  return 0;
}

int tray_loop(int blocking) {
  if (g_tray_exited) {
    return -1;
  }
  @autoreleasepool {
    NSDate *until = (blocking ? [NSDate dateWithTimeIntervalSinceNow:0.1] : [NSDate distantPast]);
    NSEvent *event = [app nextEventMatchingMask:NSEventMaskAny
                                      untilDate:until
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES];
    if (event) {
      [app sendEvent:event];
    }
  }
  return g_tray_exited ? -1 : 0;
}

void tray_update(struct tray *tray) {
  if (!tray) {
    return;
  }

  void (^updateBlock)(void) = ^{
    if (!statusItem) {
      return;
    }

    NSString *rawPath = tray->icon ? [NSString stringWithUTF8String:tray->icon] : nil;
    NSImage *image = nil;

    if (rawPath) {
      NSFileManager *fm = [NSFileManager defaultManager];
      NSString *resolvedPath = rawPath;

      if (![fm fileExistsAtPath:resolvedPath]) {
        NSString *execDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        if (execDir) {
          NSString *candidate = [[execDir stringByAppendingPathComponent:rawPath] stringByStandardizingPath];
          if ([fm fileExistsAtPath:candidate]) {
            resolvedPath = candidate;
          }
        }
      }
      if (![fm fileExistsAtPath:resolvedPath]) {
        NSString *resDir = [[NSBundle mainBundle] resourcePath];
        if (resDir) {
          NSString *cleanSub = [rawPath stringByReplacingOccurrencesOfString:@"../Resources/" withString:@""];
          cleanSub = [cleanSub stringByReplacingOccurrencesOfString:@"assets/" withString:@""];
          NSString *candidate = [[resDir stringByAppendingPathComponent:cleanSub] stringByStandardizingPath];
          if ([fm fileExistsAtPath:candidate]) {
            resolvedPath = candidate;
          } else {
            candidate = [[[resDir stringByAppendingPathComponent:@"assets"] stringByAppendingPathComponent:cleanSub] stringByStandardizingPath];
            if ([fm fileExistsAtPath:candidate]) {
              resolvedPath = candidate;
            }
          }
        }
      }

      // Check if a vector .svg version exists to ensure crisp Retina rendering
      if ([resolvedPath hasSuffix:@".png"]) {
        NSString *svgCandidate = [resolvedPath stringByReplacingOccurrencesOfString:@"-16.png" withString:@".svg"];
        svgCandidate = [svgCandidate stringByReplacingOccurrencesOfString:@"-45.png" withString:@".svg"];
        svgCandidate = [svgCandidate stringByReplacingOccurrencesOfString:@".png" withString:@".svg"];
        if ([fm fileExistsAtPath:svgCandidate]) {
          resolvedPath = svgCandidate;
        }
      }

      if ([fm fileExistsAtPath:resolvedPath]) {
        image = [[NSImage alloc] initWithContentsOfFile:resolvedPath];
      }
    }

    if (image) {
      [image setSize:NSMakeSize(18, 18)];
      BOOL isMonochrome = (tray->icon && (strstr(tray->icon, "logo-apollo") != NULL ||
                                          strstr(tray->icon, "tray") != NULL ||
                                          strstr(tray->icon, "apollo.svg") != NULL));
      [image setTemplate:isMonochrome];
      statusItem.button.image = image;
      statusItem.button.title = @"";
    } else {
      statusItem.button.image = nil;
      statusItem.button.title = @"Vibepollo";
    }

    if (tray->tooltip) {
      statusItem.button.toolTip = [NSString stringWithUTF8String:tray->tooltip];
    }

    [statusItem setMenu:_tray_menu(tray->menu)];
  };

  if ([NSThread isMainThread]) {
    updateBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), updateBlock);
  }
}

void tray_exit(void) {
  g_tray_exited = 1;
  void (^exitBlock)(void) = ^{
    if (statusBar && statusItem) {
      [statusBar removeStatusItem:statusItem];
      statusItem = nil;
    }
    if (app) {
      NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                          location:NSMakePoint(0, 0)
                                     modifierFlags:0
                                         timestamp:0
                                      windowNumber:0
                                           context:nil
                                           subtype:0
                                             data1:0
                                             data2:0];
      [app postEvent:event atStart:YES];
    }
  };

  if ([NSThread isMainThread]) {
    exitBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), exitBlock);
  }
}
