// Custom levels (ext/levels/README.md): a level made in the World editor, kept in a folder of its
// own, played in a stage slot the disc leaves empty (the developers' "scale" area: its scene files
// are not on the disc), so no stage of the game is replaced. Its Shines, blue coins and coin record
// are its own, kept per save file in a file next to the memory card's, never in the save file.
//
// A level's folder: level.ini (id, name, author, description) and stage0.szs ... stage9.szs, each
// an episode of the slot's area (a level and the levels its stage exits lead to). SMS_LEVEL names
// the folder to open as the game starts (the World editor's Play), port_levels_open one later.
#include "port_ext.h"
#include "sms_mod.h"

#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/ScenarioArchiveName.hpp>
#include <dolphin/os.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "port_host_alloc.h"

// Kept in malloc's memory, not the stage's JKR heap: these outlive stages (port_host_alloc.h).
typedef PortHostString Str;
typedef std::set<int, std::less<int>, PortHostAlloc<int> > IntSet;

extern "C" int port_dvd_lay_file(const char* path, const char* host);
extern "C" int port_dvd_exists(const char* path);
extern "C" const char* port_card_dir(void);
extern "C" void port_log(const char* fmt, ...);
extern "C" int port_levels_open(const char* dir);
extern "C" void port_levels_close(void);
extern "C" int port_levels_area(void);

namespace {

struct Level {
	Str dir, id, name, author, description;
	int shines, blues; // how many it has (level.ini's, as the World editor counts them), 0 not known
	Level() : shines(0), blues(0) {}
};
Level s_level;
bool s_open, s_tried;
int s_area = -2;                  // the slot's area (-1: none found), worked out once
std::vector<Str, PortHostAlloc<Str> > s_scenes; // its episodes' scene files, "/data/scene/scale0.szs"

// A custom level's area counts as Delfino Plaza's world (SMS_getShineStage), so its Shines take the
// game's Shine flags and its blue coins the plaza's: those flags are the level's own while Mario is
// in it, and the file's are kept aside.
const unsigned kShines = 0x10000, kShineCount = 120;
const unsigned kBlues = 0x10078, kBlueCount = 50;
const unsigned kFirst = kShines, kLast = kBlues + kBlueCount; // [kFirst, kLast)
const unsigned kShineTotal = 0x40000, kBlueTotal = 0x40001;
bool s_in;     // Mario is in the level: its flags in the file's place
bool s_guard;  // this extension is changing flags itself
int s_file[kLast - kFirst], s_fileShines, s_fileBlues, s_fileRecord;
IntSet s_shines, s_blues; // the level's own, for this save file
int s_record;
int s_saveFile = -9;
int s_gotCount;      // special Shines got this run (the HUD shows them when one more is)

unsigned recordFlag() { return 0x20005 + (unsigned)s_area; } // TFlagManager::incGoldCoinFlag's

Str trim(Str s)
{
	while (!s.empty() && isspace((unsigned char)s[s.size() - 1]))
		s.erase(s.size() - 1);
	size_t i = 0;
	while (i < s.size() && isspace((unsigned char)s[i]))
		i++;
	return s.substr(i);
}

bool isFile(const Str& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && !(st.st_mode & S_IFDIR);
}

// The slot: the first area whose episodes' scene files the disc doesn't have, the "scale" area by
// preference (TApplication's stage table, unk30).
void findArea()
{
	if (s_area != -2 || !gpApplication.unk30)
		return;
	s_area = -1;
	TNameRefPtrAryT<TNameRefAryT<TScenarioArchiveName> >& stages = *gpApplication.unk30;
	int fallback = -1;
	for (int a = 0; a < (int)stages.getChildren().size(); a++) {
		TNameRefAryT<TScenarioArchiveName>* episodes = stages.getChildren()[a];
		if (a == 15 || !episodes || episodes->getChildren().size() < 2)
			continue;
		std::vector<Str, PortHostAlloc<Str> > scenes;
		bool missing = true;
		for (size_t e = 0; e < episodes->getChildren().size(); e++) {
			const char* arc = episodes->getChildren()[e].mArcName;
			Str base = arc ? arc : "";
			size_t dot = base.rfind('.');
			if (dot != Str::npos)
				base = base.substr(0, dot);
			if (base.empty() || base == "none" || base == "option") {
				missing = false;
				break;
			}
			const Str p = "/data/scene/" + base;
			if (port_dvd_exists((p + ".szs").c_str()) || port_dvd_exists((p + ".arc").c_str())) {
				missing = false;
				break;
			}
			scenes.push_back(p + ".szs");
		}
		if (!missing)
			continue;
		if (scenes[0].find("/scale") != Str::npos || fallback < 0) {
			fallback = a;
			s_scenes = scenes;
			if (scenes[0].find("/scale") != Str::npos)
				break;
		}
	}
	s_area = fallback;
	if (s_area >= 0)
		port_log("[levels] custom levels play in area %d (%d episodes)\n", s_area, (int)s_scenes.size());
	else
		port_log("[levels] no free stage slot for custom levels\n");
}

// ---------------------------------------------------------------- the level's own save

Str savePath()
{
	const char* card = port_card_dir();
	Str dir = Str(card ? card : ".") + "/levels";
#ifdef _WIN32
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0755);
#endif
	char file[8];
	snprintf(file, sizeof file, "%c", gpApplication.mSaveFile >= 0 && gpApplication.mSaveFile <= 2 ? 'a' + gpApplication.mSaveFile : 'x');
	return dir + "/" + s_level.id + "-" + file + ".txt";
}

void loadSave()
{
	s_shines.clear();
	s_blues.clear();
	s_record = 0;
	s_saveFile = gpApplication.mSaveFile;
	FILE* f = fopen(savePath().c_str(), "r");
	if (!f)
		return;
	char word[16];
	int n;
	while (fscanf(f, "%15s %d", word, &n) == 2) {
		if (!strcmp(word, "shine") && n >= 0 && n < (int)kShineCount)
			s_shines.insert(n);
		else if (!strcmp(word, "blue") && n >= 0 && n < (int)kBlueCount)
			s_blues.insert(n);
		else if (!strcmp(word, "coins") && n > s_record)
			s_record = n;
	}
	fclose(f);
}

int s_special = -1; // the special Shines of every custom level, for this save file (-1: count again)
void writeSave()
{
	s_special = -1;
	FILE* f = fopen(savePath().c_str(), "w");
	if (!f)
		return;
	for (IntSet::const_iterator i = s_shines.begin(); i != s_shines.end(); ++i)
		fprintf(f, "shine %d\n", *i);
	for (IntSet::const_iterator i = s_blues.begin(); i != s_blues.end(); ++i)
		fprintf(f, "blue %d\n", *i);
	if (s_record > 0)
		fprintf(f, "coins %d\n", s_record);
	fclose(f);
}

// ---------------------------------------------------------------- in and out

bool levelHas(unsigned flag)
{
	if (flag >= kShines && flag < kShines + kShineCount)
		return s_shines.count((int)(flag - kShines)) != 0;
	if (flag >= kBlues && flag < kBlues + kBlueCount)
		return s_blues.count((int)(flag - kBlues)) != 0;
	return false;
}

void enter()
{
	if (s_in)
		return;
	TFlagManager* fm = TFlagManager::getInstance();
	if (!fm)
		return;
	loadSave();
	s_guard = true;
	for (unsigned f = kFirst; f < kLast; f++) {
		s_file[f - kFirst] = fm->getFlag(f);
		fm->setFlag(f, levelHas(f) ? 1 : 0);
	}
	// (the Shine and blue coin counts stay the file's: the HUD counts the game's own Shines, and the
	// level's on a row of their own, port_levels_special_total)
	s_fileShines = fm->getFlag(kShineTotal);
	s_fileBlues  = fm->getFlag(kBlueTotal);
	s_fileRecord = fm->getFlag(recordFlag());
	fm->setFlag(recordFlag(), s_record);
	s_guard = false;
	s_in    = true;
	port_log("[levels] in %s: %d Shines, %d blue coins of its own\n", s_level.name.c_str(), (int)s_shines.size(), (int)s_blues.size());
}

void leave()
{
	if (!s_in)
		return;
	TFlagManager* fm = TFlagManager::getInstance();
	s_in = false;
	if (!fm)
		return;
	s_guard = true;
	for (unsigned f = kFirst; f < kLast; f++)
		fm->setFlag(f, s_file[f - kFirst]);
	fm->setFlag(kShineTotal, s_fileShines);
	fm->setFlag(kBlueTotal, s_fileBlues);
	fm->setFlag(recordFlag(), s_fileRecord);
	s_guard = false;
	port_log("[levels] left %s: the save file's Shines back\n", s_level.name.c_str());
}

// The file's flags put back for a save to the card and back out after it (TFlagManager::save,
// saveSuccess), so a save made in a custom level saves the file as it is outside.
int s_swap[kLast - kFirst], s_swapShines, s_swapBlues, s_swapRecord;
void flagsSaving(int begin)
{
	if (!s_in)
		return;
	TFlagManager* fm = TFlagManager::getInstance();
	s_guard = true;
	if (begin) {
		for (unsigned f = kFirst; f < kLast; f++) {
			s_swap[f - kFirst] = fm->getFlag(f);
			fm->setFlag(f, s_file[f - kFirst]);
		}
		s_swapShines = fm->getFlag(kShineTotal), s_swapBlues = fm->getFlag(kBlueTotal), s_swapRecord = fm->getFlag(recordFlag());
		fm->setFlag(kShineTotal, s_fileShines), fm->setFlag(kBlueTotal, s_fileBlues), fm->setFlag(recordFlag(), s_fileRecord);
	} else {
		for (unsigned f = kFirst; f < kLast; f++)
			fm->setFlag(f, s_swap[f - kFirst]);
		fm->setFlag(kShineTotal, s_swapShines), fm->setFlag(kBlueTotal, s_swapBlues), fm->setFlag(recordFlag(), s_swapRecord);
	}
	s_guard = false;
}

// A Shine, blue coin or coin record got in the level: the level's, kept at once.
int flagChange(unsigned flag, int old, int* value)
{
	(void)old;
	if (!s_in || s_guard)
		return 0;
	if (flag == kShineTotal || flag == kBlueTotal)
		return 1; // a Shine or blue coin of the level's: not one of the file's
	if (flag >= kShines && flag < kShines + kShineCount && *value) {
		if (s_shines.insert((int)(flag - kShines)).second) {
			writeSave();
			s_gotCount++; // the HUD shows the special Shines for a while
		}
	} else if (flag >= kBlues && flag < kBlues + kBlueCount && *value) {
		if (s_blues.insert((int)(flag - kBlues)).second)
			writeSave();
	} else if (flag == recordFlag() && *value > s_record) {
		s_record = *value;
		writeSave();
	}
	return 0;
}

void stageLoading(int area, int episode)
{
	(void)episode;
	TGameSequence& cur = gpApplication.mCurrArea;
	if (s_open && s_area >= 0 && cur.unk0 == s_area)
		enter();
	else
		leave();
}

Str s_upper;
const char* stageName(int area)
{
	if (!s_open || area != s_area || s_level.name.empty())
		return nullptr;
	return s_upper.c_str();
}

void frame()
{
	if (s_tried || !gpApplication.unk30)
		return;
	s_tried = true;
	findArea();
	if (const char* dir = getenv("SMS_LEVEL"))
		if (*dir)
			port_levels_open(dir);
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name         = "levels";
		s_extension.frame        = frame;
		s_extension.stageLoading = stageLoading;
		s_extension.flagChange   = flagChange;
		s_extension.flagsSaving  = flagsSaving;
		s_extension.stageName    = stageName;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

// Opens the level in folder `dir`: its stages laid over the slot's scene files, for the next
// stage load there. 0 if it has none or there is no slot. A level open before is closed.
extern "C" int port_levels_open(const char* dir)
{
	findArea();
	if (s_area < 0 || !dir || !*dir)
		return 0;
	port_levels_close();
	Level l;
	l.dir = dir;
	while (!l.dir.empty() && (l.dir[l.dir.size() - 1] == '/' || l.dir[l.dir.size() - 1] == '\\'))
		l.dir.erase(l.dir.size() - 1);
	size_t cut = l.dir.find_last_of("/\\");
	l.id = cut == Str::npos ? l.dir : l.dir.substr(cut + 1);
	if (FILE* f = fopen((l.dir + "/level.ini").c_str(), "r")) {
		char line[512];
		while (fgets(line, sizeof line, f)) {
			Str s = line;
			size_t eq = s.find('=');
			if (eq == Str::npos || s[0] == '#' || s[0] == ';')
				continue;
			const Str k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
			if (k == "id" && !v.empty())
				l.id = v;
			else if (k == "name")
				l.name = v;
			else if (k == "author")
				l.author = v;
			else if (k == "description")
				l.description = v;
			else if (k == "shines")
				l.shines = atoi(v.c_str());
			else if (k == "blue_coins")
				l.blues = atoi(v.c_str());
		}
		fclose(f);
	}
	for (size_t i = 0; i < l.id.size(); i++)
		if (!isalnum((unsigned char)l.id[i]) && l.id[i] != '-' && l.id[i] != '_')
			l.id[i] = '-';
	int stages = 0;
	for (size_t e = 0; e < s_scenes.size(); e++) {
		char name[32];
		snprintf(name, sizeof name, "/stage%d.szs", (int)e);
		const Str host = l.dir + name;
		if (isFile(host) && port_dvd_lay_file(s_scenes[e].c_str(), host.c_str()))
			stages++;
	}
	if (!stages) {
		port_log("[levels] %s: no stage0.szs (or any stage) in it\n", l.dir.c_str());
		return 0;
	}
	s_level = l;
	s_open  = true;
	s_upper.clear();
	for (size_t i = 0; i < s_level.name.size(); i++)
		s_upper += (char)toupper((unsigned char)s_level.name[i]);
	port_log("[levels] opened %s (%s): %d stage(s), area %d\n", s_level.name.empty() ? s_level.id.c_str() : s_level.name.c_str(),
	         s_level.dir.c_str(), stages, s_area);
	return 1;
}

extern "C" void port_levels_close(void)
{
	if (!s_open)
		return;
	for (size_t i = 0; i < s_scenes.size(); i++)
		port_dvd_lay_file(s_scenes[i].c_str(), 0);
	s_open = false;
}

// The custom levels' area (-1 if none), for warping there.
extern "C" int port_levels_area(void)
{
	findArea();
	return s_area;
}

// The level Mario is in (1) or not (0); its name; what of its own he has and how many it has.
extern "C" int port_levels_in(void) { return s_in ? 1 : 0; }
extern "C" const char* port_levels_name(void) { return s_open ? s_upper.c_str() : ""; }
extern "C" void port_levels_counts(int* shines, int* allShines, int* blues, int* allBlues)
{
	if (shines)
		*shines = (int)s_shines.size();
	if (allShines)
		*allShines = s_level.shines > (int)s_shines.size() ? s_level.shines : (int)s_shines.size();
	if (blues)
		*blues = (int)s_blues.size();
	if (allBlues)
		*allBlues = s_level.blues > (int)s_blues.size() ? s_level.blues : (int)s_blues.size();
}
// A save file flag as the file has it, its own where the level has put its own in their place.
extern "C" int port_levels_file_flag(unsigned flag)
{
	TFlagManager* fm = TFlagManager::getInstance();
	if (s_in && flag >= kFirst && flag < kLast)
		return s_file[flag - kFirst];
	if (s_in && flag == kShineTotal)
		return s_fileShines;
	if (s_in && flag == kBlueTotal)
		return s_fileBlues;
	return fm ? fm->getFlag(flag) : 0;
}

// Every custom level's special Shines got with this save file (the levels' files next to the
// memory card), for the HUD's row of them.
extern "C" int port_levels_special_total(void)
{
	static int s_forFile = -9;
	if (s_forFile != gpApplication.mSaveFile)
		s_special = -1, s_forFile = gpApplication.mSaveFile;
	if (s_special >= 0)
		return s_special;
	s_special = 0;
	const char* card = port_card_dir();
	const Str dir = Str(card ? card : ".") + "/levels";
	char tail[8];
	snprintf(tail, sizeof tail, "-%c.txt", gpApplication.mSaveFile >= 0 && gpApplication.mSaveFile <= 2 ? 'a' + gpApplication.mSaveFile : 'x');
	if (DIR* d = opendir(dir.c_str())) {
		while (dirent* e = readdir(d)) {
			const Str n = e->d_name;
			if (n.size() <= strlen(tail) || n.compare(n.size() - strlen(tail), strlen(tail), tail) != 0)
				continue;
			if (FILE* f = fopen((dir + "/" + n).c_str(), "r")) {
				char word[16];
				int v;
				while (fscanf(f, "%15s %d", word, &v) == 2)
					if (!strcmp(word, "shine"))
						s_special++;
				fclose(f);
			}
		}
		closedir(d);
	}
	return s_special;
}

// How many special Shines were got since the game started: the HUD shows its row of them once the
// HUD is back after one more (the Shine's own scene and the stage it leads to come first).
extern "C" int port_levels_special_got(void) { return s_gotCount; }

// ---------------------------------------------------------------- the levels installed

namespace {

struct Installed {
	Str folder, id, name, author, description;
	int shines, blues;
};
std::vector<Installed, PortHostAlloc<Installed> > s_installed;

bool byName(const Installed& a, const Installed& b) { return a.name < b.name; }

// A level folder's level.ini, or false.
bool readIni(const Str& folder, Installed& out)
{
	if (!isFile(folder + "/stage0.szs"))
		return false;
	out.folder = folder;
	size_t cut = folder.find_last_of("/\\");
	out.id     = cut == Str::npos ? folder : folder.substr(cut + 1);
	out.name   = out.id;
	out.shines = out.blues = 0;
	if (FILE* f = fopen((folder + "/level.ini").c_str(), "r")) {
		char line[512];
		while (fgets(line, sizeof line, f)) {
			Str t = line;
			size_t eq = t.find('=');
			if (eq == Str::npos || t[0] == '#' || t[0] == ';')
				continue;
			const Str k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
			if (k == "id" && !v.empty())
				out.id = v;
			else if (k == "name" && !v.empty())
				out.name = v;
			else if (k == "author")
				out.author = v;
			else if (k == "description")
				out.description = v;
			else if (k == "shines")
				out.shines = atoi(v.c_str());
			else if (k == "blue_coins")
				out.blues = atoi(v.c_str());
		}
		fclose(f);
	}
	return true;
}

// The levels folder (SMS_LEVELS_DIR, the launcher's), looked through again.
void scan()
{
	s_installed.clear();
	const char* root = getenv("SMS_LEVELS_DIR");
	if (!root || !*root)
		return;
	if (DIR* d = opendir(root)) {
		while (dirent* e = readdir(d)) {
			if (e->d_name[0] == '.')
				continue;
			Installed l;
			if (readIni(Str(root) + "/" + e->d_name, l))
				s_installed.push_back(l);
		}
		closedir(d);
	}
	std::sort(s_installed.begin(), s_installed.end(), byName);
}

void copyText(char* to, size_t n, const Str& from)
{
	snprintf(to, n, "%s", from.c_str());
}

// A level's own save for this save file: how many Shines and blue coins.
void savedCounts(const Str& id, int* shines, int* blues)
{
	*shines = *blues = 0;
	const char* card = port_card_dir();
	char file[8];
	snprintf(file, sizeof file, "%c", gpApplication.mSaveFile >= 0 && gpApplication.mSaveFile <= 2 ? 'a' + gpApplication.mSaveFile : 'x');
	Str clean = id;
	for (size_t i = 0; i < clean.size(); i++)
		if (!isalnum((unsigned char)clean[i]) && clean[i] != '-' && clean[i] != '_')
			clean[i] = '-';
	FILE* f = fopen((Str(card ? card : ".") + "/levels/" + clean + "-" + file + ".txt").c_str(), "r");
	if (!f)
		return;
	char word[16];
	int n;
	while (fscanf(f, "%15s %d", word, &n) == 2) {
		if (!strcmp(word, "shine"))
			(*shines)++;
		else if (!strcmp(word, "blue"))
			(*blues)++;
	}
	fclose(f);
}

} // namespace

extern "C" int port_levels_count(void)
{
	scan();
	return (int)s_installed.size();
}

extern "C" int port_levels_info(int index, SmsLevelInfo* out)
{
	if (!out || index < 0 || index >= (int)s_installed.size())
		return 0;
	const uint32_t size = out->size && out->size < sizeof(SmsLevelInfo) ? out->size : (uint32_t)sizeof(SmsLevelInfo);
	SmsLevelInfo info;
	memset(&info, 0, sizeof info);
	const Installed& l = s_installed[index];
	info.size = size;
	copyText(info.id, sizeof info.id, l.id);
	copyText(info.name, sizeof info.name, l.name);
	copyText(info.author, sizeof info.author, l.author);
	copyText(info.description, sizeof info.description, l.description);
	copyText(info.folder, sizeof info.folder, l.folder);
	info.shines     = l.shines;
	info.blue_coins = l.blues;
	savedCounts(l.id, &info.got_shines, &info.got_blue_coins);
	memcpy(out, &info, size);
	return 1;
}

// Mario to the index-th level, as a stage exit takes him: the level opened, the next stage its first.
extern "C" int port_levels_enter(int index)
{
	if (index < 0 || index >= (int)s_installed.size() || !gpMarDirector)
		return 0;
	if (!port_levels_open(s_installed[index].folder.c_str()))
		return 0;
	gpMarDirector->setNextStage((u16)(((s_area + 1) << 8) | 0), 0);
	return 1;
}

extern "C" int port_levels_current(void)
{
	if (!s_in)
		return -1;
	for (size_t i = 0; i < s_installed.size(); i++)
		if (s_installed[i].folder == s_level.dir)
			return (int)i;
	return -1;
}

// The open level's name as written (its level.ini's), or "".
extern "C" const char* port_levels_title(void) { return s_open ? s_level.name.c_str() : ""; }
