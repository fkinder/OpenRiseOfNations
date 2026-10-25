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
#include "lobby.h"
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
// Multiplayer (lobby.h): the room started the game; `setup` is the final one (the host's seed, human seats = connected players).
// `save`: a saved multiplayer game the session continues (null for a new game).
- (void)frontEndPlayMultiplayer:(std::shared_ptr<mp::Room>)room setup:(const ron::sim::RandomGameSetup&)setup save:(std::shared_ptr<std::vector<uint8_t>>)save;
// A scenario game: the room's table carries the scenario and its seats (Room::read_scenario).
- (void)frontEndPlayMultiplayerScenario:(std::shared_ptr<mp::Room>)room;
// A Conquer the World battle of the host's campaign: `session` is the host's (null on a guest, which builds its copy from the campaign file the
// host sent). A guest asks the host side what to do with the campaign the host sent after the battle.
- (void)frontEndPlayMultiplayerCtw:(std::shared_ptr<mp::Room>)room session:(std::shared_ptr<ctwui::Session>)session;
- (NSString*)frontEndCtwReceived:(const std::vector<uint8_t>&)campaign outcome:(int)outcome;
@end

@interface FrontEnd : NSObject <MTKViewDelegate>
@property(nonatomic, weak) id<FrontEndHost> host;
@property(nonatomic, weak) MTKView* view;
@property(nonatomic, readonly) NSString* gameDir;
- (instancetype)initWithDevice:(id<MTLDevice>)device gameDir:(NSString*)dir error:(NSString* __autoreleasing*)error;
// main, tutorials, skirmish, skirmish-setup, scenarios, multiplayer, mp-lobby; "quit-popup" = the Main Menu with the Quit popup open.
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
// Headless (--menu mp-lobby): hosts a room on `port` and lets `guests` (names) join it from threads of this process, so the lobby screen can be
// looked at; each guest says a line in the chat and gets ready. Returns once they are all in.
- (BOOL)mpDemoPort:(int)port guests:(NSArray<NSString*>*)guests;
// Headless test: the host presses Start on the lobby of the demo room (every guest is made ready first).
- (void)mpDemoStart;
// Headless test (RON_MP_DEMO_UI): a second front end joins the room of `hostFe` as a real guest (its lobby screen, its pump), the connect running on a thread
// while the host's front end keeps pumping; `mpDemoState` says which screen it is on and what the room holds (tests read it after each scripted click).
- (BOOL)mpDemoJoin:(int)port name:(NSString*)name pumping:(FrontEnd*)hostFe;
- (NSString*)mpDemoState;
// Window tests through the real app delegate (RON_MP_APP_TEST, main.mm): the centre of a lobby control in view points ("host", "join", "load", "scenario",
// "ctw", "start", "back"; CGPointZero when it is not on the screen), to click it with mouseMoved / mouseDown / mouseUp; and the campaign map's Play press.
// RON_MP_PICK=PATH answers the lobby's file chooser (Load Saved Game, Scenario) without the modal panel; RON_MP_NAME / RON_MP_PORT preset the connect screen.
- (CGPoint)mpDemoPoint:(NSString*)which;
- (void)mpDemoCtwPlay;
- (void)ctwShowSession:(std::shared_ptr<ctwui::Session>)session sub:(NSString*)sub select:(NSString*)country;
// Back from a multiplayer game (a Conquer the World battle): the room is a lobby again. The host (`ctw` YES) returns to the campaign map with the
// room kept (the next battle can be played with the same players), a guest to the lobby, with `message` as a line in the chat.
- (void)mpResume:(std::shared_ptr<mp::Room>)room ctw:(BOOL)ctw message:(NSString*)message;
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
