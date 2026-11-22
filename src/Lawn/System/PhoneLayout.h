/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#ifndef __PHONELAYOUT_H__
#define __PHONELAYOUT_H__

#include <vector>

// The layout of a level on a wide phone screen, after the phone versions of the game: the game's screen as tall as the
// window and against the right side of the safe area, with the house of the level's backdrop left of it; the seed
// packets as wide cards (the plant on the left, the cost in the bottom right corner) in one column down the left edge;
// along the top, right of the cards, the sun counter (its sun beside its number) and the shovel; the progress meter
// beside the menu button in the top right corner.
//
// The game still draws its 800x600 screen, but without the seed bank, the shovel, the progress meter and the menu
// button, and the layout shows it in the window over the backdrop. Those the game draws into the window itself, at the
// window's resolution, where the layout puts them (overlay tiles). A tap is mapped back through the topmost tile under
// it: on a card to its packet, on the shovel or the menu button to it, on the game's screen to that place of it, so
// every part of the game handles it as before.
//
// Only geometry, without the game, GL or SDL, so that tests can check it (tests/phone_layout_test.cpp).
namespace PhoneLayout
{

struct Box
{
	double	mX = 0;
	double	mY = 0;
	double	mWidth = 0;
	double	mHeight = 0;

	double	Right() const { return mX + mWidth; }
	double	Bottom() const { return mY + mHeight; }
	bool	Empty() const { return mWidth <= 0 || mHeight <= 0; }
	bool	Contains(double theX, double theY) const { return theX >= mX && theX < Right() && theY >= mY && theY < Bottom(); }
};

enum class Kind
{
	Screen,		// the game's screen (mSource in its 800x600 coordinates), scaled alike both ways
	Backdrop,	// a piece of the backdrop image of the level (mSource in the game's coordinates all the same); no tap goes to it
	// drawn into the window by the game:
	Card,		// the card of packet mIndex; a tap on it goes to the packet (mSource)
	SunCounter,	// the sun counter; it takes taps, and does nothing with them
	Meter,		// the progress meter (mSource: where the game draws it in its screen); it takes taps, and does nothing with them
	Menu,		// the menu button (mSource: where it is in the game's screen); a tap on it goes to the button
	Shovel,		// the shovel in its bank (mSource: where it is in the game's screen); a tap on it goes to the shovel
	Conveyor,	// the conveyor belt the cards ride up, in conveyor belt levels; it takes taps, and does nothing with them
	Weather,	// over a piece of the backdrop, the darkness the game draws over its screen (the stormy night); no tap goes to it
	Blur,		// the game's screen (mSource: a piece of it) blurred and dimmed, as the background of a framed screen; no tap goes to it
	Surround,	// what goes on around the game's screen (where mDest is, mSource of it) on a screen without a lawn behind, of
				// the kind mIndex (a Surround), drawn into the window; it takes no taps
	Beyond,		// what of the level lies past the edges of the game's screen (zombies coming in from the street, ...), drawn
				// over the backdrop under the screen (where mDest is, mSource of it); no taps
	Band,		// the dark band behind a message across the game's screen (where mDest is, mSource of it), going on across
				// the window; no taps
	Title,		// the title over the whole window: its picture covering it, its logo and loading bar where the game's screen
				// (mDest) has them; taps go to the screen under it
	StoreFront,	// over the store's screen (where mDest is, mSource of it): the far end of the car, which its picture cuts at
				// the screen's edge, fading into the sky beyond; taps go to the screen under it
	Edge,		// past an end of the backdrop's image (mSource: its column at that end, in the game's coordinates), out to the
				// window's edge: the image's edge going on, blurred, and darkening towards the window's edge; no taps
	Board,		// down the left edge of the window, from its top to its bottom, the wooden board of the seed bank turned
				// upright, which the cards lie on (mSource: its size in the board's units, the cards' scale); over where the
				// level's own picture ends short of the window's edge; mIndex 1: one down the right edge, its rim on its
				// left (beside a garden, whose picture ends at the screen's edges); mIndex 2: plain black, left of the board
				// under the notch or the Dynamic Island; it takes taps and does nothing with them
	Cursor,		// what the player holds (a plant, the shovel, the cob cannon's target), over everything, in the game's screen's
				// coordinates (where mDest is, mSource of it) past its edges too, following the finger; taps go to what is under it
};

// Where something goes in the window (mDest, in window points), and what of the game it shows or stands for (mSource,
// in the game's coordinates).
struct Tile
{
	Box		mSource;
	Box		mDest;
	Kind	mKind = Kind::Screen;
	int		mIndex = -1;	// of the packet, for a card

	bool	Overlay() const { return mKind != Kind::Screen && mKind != Kind::Backdrop && mKind != Kind::Blur; }
	bool	TakesTaps() const
	{
		return mKind != Kind::Backdrop && mKind != Kind::Weather && mKind != Kind::Blur && mKind != Kind::Surround && mKind != Kind::Beyond &&
			mKind != Kind::Band && mKind != Kind::Title && mKind != Kind::StoreFront && mKind != Kind::Edge && mKind != Kind::Cursor;
	}
	bool	TakesNoTap() const { return mKind == Kind::SunCounter || mKind == Kind::Meter || mKind == Kind::Conveyor || mKind == Kind::Board; }
};

// What goes on around the game's screen on a screen without a lawn behind (PhoneLayoutDraw draws it)
enum Surround
{
	SURROUND_NONE,
	SURROUND_MENU,		// the main menu's scenery, and past its edges the achievements' and Zombatar's own when it slides to them
	SURROUND_STORE,		// the store's fence going on, and Crazy Dave whole
	SURROUND_PAGE,		// a page (the almanac, the lists of mini-games, an award) laid over the main menu's scenery, still
	SURROUND_GARDEN,	// a garden (the Zen Garden's, the aquarium): its picture ends at the screen's edges, boards as the cards' beside it
	SURROUND_NOTE,		// a zombie's note: the picture behind it (drawn larger than the screen) going on
	SURROUND_TREE,		// the Tree of Wisdom's sky and hill going on, its clouds drifting past
	SURROUND_CREDITS,	// the credits going on past the screen's edges as they draw themselves (their scenes are larger
						// than the screen), on their black
};

struct Input
{
	double				mScreenWidth = 0;	// the window, in points
	double				mScreenHeight = 0;
	Box					mSafeArea;			// the part of the window clear of the notch or Dynamic Island, in points

	Box					mLawn;				// the game's screen
	Box					mBackdrop;			// where the backdrop image of the level lies, in the game's coordinates; empty without

	Box					mMenuButton;		// in the game's screen; empty when it is not shown
	Box					mSunCounter;		// empty without one
	Box					mShovel;			// empty without a shovel
	Box					mMeter;				// the progress meter with its flags; empty when it is not shown
	std::vector<Box>	mPackets;			// where the packets are (what a tap on a card goes to)
	// a conveyor belt level: where the game has the belt (empty in other levels), and for each packet where it is along
	// the belt, in slots from the belt's start (where the packets ride to) to its end at CONVEYOR_SLOTS; the cards ride up
	// a belt down the left edge instead of standing in a column
	Box					mConveyorBelt;
	std::vector<double>	mPacketSlots;
	bool				mDarkWeather = false;	// the game darkens all of its screen (the stormy night): the backdrop too
	int					mSurround = SURROUND_NONE;	// without a lawn behind (framed, or in a level without a backdrop): what goes
													// on around the game's screen
	bool				mTitle = false;				// the title is shown: drawn over the whole window
	double				mCardSlots = 6;				// framed with a lawn behind: the cards the level has (or the slots of its
	double				mCardPad = 0;				// conveyor belt, and its pad), for the board they lie on in the level
	bool				mBeyond = false;		// with the backdrop, what of the level lies past the screen's edges is drawn over it,
												// and the band behind a message goes on across the window
};

constexpr double	MIN_ASPECT = 1.65;			// narrower screens (iPad, 4:3 and 16:10) keep the 800x600 screen
constexpr double	MIN_COLUMN_WIDTH = 80;		// points left of the game's screen for the cards, at the least
constexpr double	EDGE_MARGIN = 4;			// points between the controls and the edges of the safe area
constexpr double	GAP = 5;					// points between the cards and what is right of them, and along the top
constexpr double	CARD_GAP = 3;				// points between the cards
// the cards and the sun counter in units of their own, drawn at one scale: at most MAX_CARD_HEIGHT of the window high,
// less when the cards would not fit down the window
constexpr double	CARD_WIDTH = 88;
constexpr double	CARD_HEIGHT = 50;
constexpr double	MAX_CARD_HEIGHT = 0.11;
constexpr double	SUN_COUNTER_WIDTH = 118;
constexpr double	SUN_COUNTER_HEIGHT = 42;
constexpr double	MIN_MENU_HEIGHT = 36;		// points high, the menu button, at the least
constexpr double	CONVEYOR_SLOTS = 10;		// the length of the conveyor belt, in packets
constexpr double	CONVEYOR_PAD = 5;			// card units between the cards and the sides of the belt
// the middle of Crazy Dave and his car in the store's screen (he stands from about 90 left of it, the car reaches its
// right edge), which goes in the middle of the window
constexpr double	STORE_MIDDLE = 355;
constexpr double	HOUSE_WIDTH = 220;			// the backdrop's part left of the game's screen in a level (the house)

// The tiles, drawn in this order: the backdrop (when there is one: its part left of the game's screen, then its part
// right of it, both at the screen's scale, in line with it; past its right end, out to the window's edge, its edge going
// on, darkening: Edge), the game's screen, the weather over the backdrop, the board the cards lie on (from the window's
// left edge past the cards, and past where the level's own picture ends on the left: the backdrop's image, or the game's
// screen without one), the shovel, the menu button, the
// progress meter, the sun counter, the conveyor belt, the cards, what the player holds. Empty when the screen is not wide enough for the layout
// to gain anything; the game then shows its 800x600 screen as usual.
std::vector<Tile>	Compute(const Input& theInput);

// Framed, outside the level layout (menus, the seed chooser, dialogs, ...), on a screen as wide as Compute needs, so
// that no black shows: with a lawn behind (mBackdrop), the game's screen where Compute puts it; else as tall as the
// window in the middle of the safe area (the Tree of Wisdom's too, and a garden's, with boards as the cards' at both
// sides; the store's moved so that
// Crazy Dave and his car are in the middle: STORE_MIDDLE), with what goes on around it (mSurround: the main menu's
// scenery, the store's, ...); the title (mTitle) drawn over the whole window, its picture covering it; the store's car
// fading out at its cut (StoreFront); with a lawn behind, the board the cards lie on in a level, where a level has it
// (moving with the backdrop when the board pans, sliding off the window's edge when it pans to the street). Under all a
// blurred copy of the game's screen that covers the window, where nothing else does. Uses mScreenWidth, mScreenHeight, mSafeArea, mLawn (the game's screen), mBackdrop, mBeyond,
// mDarkWeather, mSurround and mTitle. Empty on narrower screens.
std::vector<Tile>	ComputeFramed(const Input& theInput);

// The point of the game's screen under a window point, through the topmost tile there that is not the backdrop; false
// when there is none, or when it is one that takes taps and does nothing with them.
bool				WindowToGame(const std::vector<Tile>& theTiles, double theX, double theY, double& theGameX, double& theGameY);

}

#endif
