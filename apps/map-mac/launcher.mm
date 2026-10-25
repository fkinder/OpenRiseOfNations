// Game-folder detection and the front end's map lists (Tutorials, Skirmish, Scenarios). The menu screens themselves are
// drawn in the game window (frontend.mm); only the first-run folder picker is a native panel.
#import "launcher.h"

#include "prefs.hpp"

namespace launcher {

NSString* const kDefGameDir = @"GameDirectory";
NSString* const kDefDifficulty = @"Difficulty";
NSString* const kDefScreen = @"MenuScreen";

bool looks_like_install(NSString* dir) {
	if (dir.length == 0) return false;
	NSFileManager* fm = NSFileManager.defaultManager;
	BOOL isDir = NO;
	if (![fm fileExistsAtPath:dir isDirectory:&isDir] || !isDir) return false;
	bool data = false, scen = false, exe = false;
	for (NSString* e in [fm contentsOfDirectoryAtPath:dir error:nil]) {
		if ([e caseInsensitiveCompare:@"Data"] == NSOrderedSame) data = true;
		if ([e caseInsensitiveCompare:@"scenario"] == NSOrderedSame) scen = true;
		if ([e caseInsensitiveCompare:@"riseofnations.exe"] == NSOrderedSame) exe = true;  // the exe next to a Data folder is enough
	}
	return data && (scen || exe);
}

NSString* guess_install() {
	NSString* home = NSHomeDirectory();
	NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithObjects:
	    [home stringByAppendingPathComponent:@"Games/RiseOfNations"],
	    [home stringByAppendingPathComponent:@"Games/Rise of Nations"],
	    [home stringByAppendingPathComponent:@"Library/Application Support/Steam/steamapps/common/Rise of Nations"], nil];
	NSString* bottles = [home stringByAppendingPathComponent:@"Library/Application Support/CrossOver/Bottles"];
	for (NSString* b in [NSFileManager.defaultManager contentsOfDirectoryAtPath:bottles error:nil] ?: @[]) {
		[candidates addObject:[NSString pathWithComponents:@[ bottles, b, @"drive_c/Program Files (x86)/Steam/steamapps/common/Rise of Nations" ]]];
	}
	for (NSString* c in candidates) {
		if (looks_like_install(c)) return c;
	}
	return @"";
}

NSString* saved_or_guessed_install() {
	// The preferences file's game_dir (written by the first-run picker), then the NSUserDefaults profile of older builds, then a guess.
	const std::string& fromPrefs = prefs::get().game_dir;
	if (!fromPrefs.empty() && looks_like_install(@(fromPrefs.c_str()))) return @(fromPrefs.c_str());
	NSString* dir = [NSUserDefaults.standardUserDefaults stringForKey:kDefGameDir];
	return looks_like_install(dir) ? dir : guess_install();
}

void remember_install(NSString* dir) {
	if (!looks_like_install(dir)) return;
	[NSUserDefaults.standardUserDefaults setObject:dir forKey:kDefGameDir];
	if (prefs::get().game_dir != dir.UTF8String) prefs::save_game_dir(dir.UTF8String);
}

static NSOpenPanel* folder_panel(NSString* start) {
	NSOpenPanel* p = [NSOpenPanel openPanel];
	p.canChooseDirectories = YES;
	p.canChooseFiles = NO;
	p.allowsMultipleSelection = NO;
	p.prompt = @"Choose";
	p.message = @"Choose your Rise of Nations: Extended Edition folder (the one with Data and scenario in it).";
	if (start.length) p.directoryURL = [NSURL fileURLWithPath:start];
	return p;
}

static void not_an_install(NSString* dir) {
	NSAlert* a = [NSAlert new];
	a.messageText = @"This is not a Rise of Nations folder";
	a.informativeText = [NSString stringWithFormat:@"%@ does not contain the game's Data and scenario folders. Choose the "
	                                               @"folder of your Rise of Nations: Extended Edition install.", dir];
	[a runModal];
}

NSString* ask_for_install(NSString* reason) {
	for (;;) {
		NSAlert* a = [NSAlert new];
		a.messageText = @"Where is Rise of Nations?";
		a.informativeText = reason ?: @"OpenRiseOfNations plays with the art and data of your own copy of Rise of Nations: "
		                               @"Extended Edition. Choose the game's folder (the one with Data and scenario in it). "
		                               @"The README explains how to get the game files onto a Mac.";
		[a addButtonWithTitle:@"Choose Folder…"];
		[a addButtonWithTitle:@"Quit"];
		if ([a runModal] != NSAlertFirstButtonReturn) return nil;
		NSOpenPanel* p = folder_panel(NSHomeDirectory());
		if ([p runModal] != NSModalResponseOK) continue;
		if (looks_like_install(p.URL.path)) return p.URL.path;
		not_an_install(p.URL.path);
	}
}

NSString* choose_install(NSString* current) {
	NSOpenPanel* p = folder_panel(current.stringByDeletingLastPathComponent);
	if ([p runModal] != NSModalResponseOK) return nil;
	if (looks_like_install(p.URL.path)) return p.URL.path;
	not_an_install(p.URL.path);
	return nil;
}

// The path of `rel` inside the install, matching folder and file names case-insensitively; nil if missing.
static NSString* find_in_install(NSString* dir, NSString* rel) {
	NSString* cur = dir;
	for (NSString* part in [rel componentsSeparatedByString:@"/"]) {
		NSString* match = nil;
		for (NSString* e in [NSFileManager.defaultManager contentsOfDirectoryAtPath:cur error:nil]) {
			if ([e caseInsensitiveCompare:part] == NSOrderedSame) {
				match = e;
				break;
			}
		}
		if (!match) return nil;
		cur = [cur stringByAppendingPathComponent:match];
	}
	return [cur substringFromIndex:dir.length + 1];
}

std::vector<Entry> entries_for(Mode m, NSString* dir) {
	std::vector<Entry> out;
	auto add = [&](const char* title, const char* detail, NSString* rel) {
		if (NSString* p = find_in_install(dir, rel)) out.push_back({title, detail, p.UTF8String});
	};
	if (m == Mode::Tutorials) {
		add("Tutorial 1: Getting Around", "Select and move units, scroll and zoom the map.", @"scenario/Tutorial1/tut1.scx");
		add("Tutorial 2: Building", "Citizens, military buildings and building in groups.", @"scenario/Tutorial2/tut2.scx");
		add("Tutorial 3: Combat", "Lead your army against a city and its defenders.", @"scenario/Tutorial3/tut3.scx");
		add("Tutorial 4: Cities", "The alarm, the library and founding new cities.", @"scenario/Tutorial4/tut4.scx");
		add("Tutorial 5: Growing Your Nation", "Gather resources, build farms and advance an age.", @"scenario/Tutorial5/tut5.scx");
		add("Tutorial 6: Diplomacy", "War, peace and alliances.", @"scenario/Tutorial6/tut6.scx");
	} else if (m == Mode::Skirmish) {
		add("Skirmish", "Ready-made map with computer opponents.", @"scenario/Custom/skirmish/skirmish.scx");
		add("Caribbean", "Islands and coasts.", @"scenario/Custom/caribbean/caribbean.scx");
		add("Europe", "Eight nations across Europe.", @"scenario/Custom/europe/europe.scx");
		add("Italy", "A crowded peninsula.", @"scenario/Custom/italy_mp/italy_mp.scx");
		add("Capture", "Take the enemy's cities.", @"scenario/Custom/capture/capture.scx");
		add("Missile", "Modern-age warfare.", @"scenario/Custom/missile/missile.scx");
	} else {
		struct { NSString* file; const char* title; } chess[] = {
		    {@"ceaging1", "Chess Exercise: Ages 1"}, {@"ceaging2", "Chess Exercise: Ages 2"},
		    {@"ceaging3", "Chess Exercise: Ages 3"}, {@"cegeneral3", "Chess Exercise: General"},
		    {@"cehotkey1", "Chess Exercise: Hotkeys"}, {@"cemicrowars", "Chess Exercise: Micro Wars"},
		    {@"ceraiding1", "Chess Exercise: Raiding"}, {@"cewonder", "Chess Exercise: Wonder"}};
		for (const auto& c : chess)
			add(c.title, "A short challenge.", [NSString stringWithFormat:@"scenario/Chess Exercise/%@.scx", c.file]);
		// Conquer the World battle maps, played on their own (the campaign map is not supported yet).
		NSString* conquest = find_in_install(dir, @"conquest");
		if (conquest) {
			NSString* root = [dir stringByAppendingPathComponent:conquest];
			NSMutableArray<NSString*>* maps = [NSMutableArray array];
			for (NSString* rel in [NSFileManager.defaultManager enumeratorAtPath:root]) {
				if ([rel.pathExtension caseInsensitiveCompare:@"scx"] == NSOrderedSame) [maps addObject:rel];
			}
			[maps sortUsingSelector:@selector(caseInsensitiveCompare:)];
			for (NSString* rel in maps) {
				NSString* campaign = rel.pathComponents.firstObject;
				NSString* title = [NSString stringWithFormat:@"%@: %@", campaign, rel.lastPathComponent.stringByDeletingPathExtension];
				out.push_back({title.UTF8String, "Campaign battle (experimental: some end early without the campaign map).",
				               [conquest stringByAppendingPathComponent:rel].UTF8String});
			}
		}
	}
	return out;
}

}  // namespace launcher
