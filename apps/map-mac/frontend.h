// The front end (docs/ui-port.md "Front end"): the Main Menu, the Tutorials / Skirmish / Scenarios lists, the AI
// difficulty drop-down and the Quit popup, drawn in the game window with the install's own front-end art (topmenu.xml:
// the animated TopMenu backdrop, the TopMenuSelection ribbon, bronze frame and buttons, the bump-mapped EE logo;
// interface.xml: popup buttons, scroll bars) and fonts. Laid out on a 1024 x 768 canvas, scaled to fit the window
// and centred; the backdrop covers the whole window.
#pragma once

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "assets/files.hpp"
#include "ctw_ui.h"
#include "sim/newgame.hpp"

// What the menu asks of the app.
@protocol FrontEndHost <NSObject>
// Play a scenario (path relative to the install); difficulty 0..5, or -1 to keep the scenario's default (tutorials).
- (void)frontEndPlay:(NSString*)scenario difficulty:(int)difficulty;
// Play a random-map skirmish built by the setup screen (sim/newgame.hpp); the seed is already drawn.
- (void)frontEndPlayRandom:(const ron::sim::RandomGameSetup&)setup;
- (void)frontEndChangeFolder;
- (void)frontEndQuit;
@optional
// Conquer the World (ctw_ui.h): the campaign in progress, kept by the host across the battles (the front end is rebuilt around
// a game), and the request to play the battle the session prepared.
- (std::shared_ptr<ctwui::Session>)frontEndCtwSession;
- (void)frontEndPlayCtwBattle:(std::shared_ptr<ctwui::Session>)session;
@end

@interface FrontEnd : NSObject <MTKViewDelegate>
@property(nonatomic, weak) id<FrontEndHost> host;
@property(nonatomic, weak) MTKView* view;
@property(nonatomic, readonly) NSString* gameDir;
- (instancetype)initWithDevice:(id<MTLDevice>)device gameDir:(NSString*)dir error:(NSString* __autoreleasing*)error;
// main, tutorials, skirmish, skirmish-setup, scenarios; "quit-popup" = the Main Menu with the Quit popup open.
- (void)showScreen:(NSString*)screen;
// A message box without buttons (e.g. "Loading…") until cleared with nil; an error box with an OK button.
- (void)setBusy:(NSString*)text;
- (void)showError:(NSString*)text;
// The window is up: real time drives the backdrop animation, the menu music plays (Data/sound.xml INTROS).
- (void)startInteractive;
- (void)stopAudio;
// Offscreen render of the current screen (w x h drawable pixels, `scale` drawable pixels per point).
- (BOOL)snapshotTo:(NSString*)path width:(int)w height:(int)h scale:(float)scale;
// Headless (--ctw): opens the strategic map of `session` ("" / news / cards / diplomacy / attack / menu), `country` selected.
- (void)ctwShowSession:(std::shared_ptr<ctwui::Session>)session sub:(NSString*)sub select:(NSString*)country;
// Input in view points with a top-left origin.
- (void)mouseMoved:(CGPoint)p;
- (void)mouseDown:(CGPoint)p clicks:(NSInteger)clicks;
- (void)mouseDragged:(CGPoint)p;
- (void)mouseUp:(CGPoint)p;
- (void)scrollWheel:(CGFloat)dy at:(CGPoint)p;
- (void)keyDown:(NSEvent*)event;
- (NSCursor*)cursor;
@end

// The game's cursor `cid` (a Data/cursors.xml row) as an NSCursor (arrow when missing).
NSCursor* ron_game_cursor(const ron::GameFiles& files, int cid);
