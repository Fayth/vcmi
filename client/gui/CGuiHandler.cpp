#include "StdInc.h"
#include "CGuiHandler.h"

#include <SDL.h>

#include "CIntObject.h"
#include "CCursorHandler.h"
#include "CAnimation.h"

#include "../CGameInfo.h"
#include "../../lib/CThreadHelper.h"
#include "../../lib/CConfigHandler.h"
#include "../CMT.h"
#include "../CPlayerInterface.h"

extern std::queue<SDL_Event> events;
extern boost::mutex eventsM;

boost::thread_specific_ptr<bool> inGuiThread;

SObjectConstruction::SObjectConstruction( CIntObject *obj )
:myObj(obj)
{
	GH.createdObj.push_front(obj);
	GH.captureChildren = true;
}

SObjectConstruction::~SObjectConstruction()
{
	assert(GH.createdObj.size());
	assert(GH.createdObj.front() == myObj);
	GH.createdObj.pop_front();
	GH.captureChildren = GH.createdObj.size();
}

SSetCaptureState::SSetCaptureState(bool allow, ui8 actions)
{
	previousCapture = GH.captureChildren;
	GH.captureChildren = false;
	prevActions = GH.defActionsDef;
	GH.defActionsDef = actions;
}

SSetCaptureState::~SSetCaptureState()
{
	GH.captureChildren = previousCapture;
	GH.defActionsDef = prevActions;
}

static inline void
processList(const ui16 mask, const ui16 flag, std::list<CIntObject*> *lst, std::function<void (std::list<CIntObject*> *)> cb)
{
	if (mask & flag)
		cb(lst);
}

void CGuiHandler::processLists(const ui16 activityFlag, std::function<void (std::list<CIntObject*> *)> cb)
{
	processList(CIntObject::LCLICK,activityFlag,&lclickable,cb);
	processList(CIntObject::RCLICK,activityFlag,&rclickable,cb);
	processList(CIntObject::HOVER,activityFlag,&hoverable,cb);
	processList(CIntObject::MOVE,activityFlag,&motioninterested,cb);
	processList(CIntObject::KEYBOARD,activityFlag,&keyinterested,cb);
	processList(CIntObject::TIME,activityFlag,&timeinterested,cb);
	processList(CIntObject::WHEEL,activityFlag,&wheelInterested,cb);
	processList(CIntObject::DOUBLECLICK,activityFlag,&doubleClickInterested,cb);
	
	#ifndef VCMI_SDL1
	processList(CIntObject::TEXTINPUT,activityFlag,&textInterested,cb);
	#endif // VCMI_SDL1
}

void CGuiHandler::fadeInNewScreen(IShowActivatable * newScreen)
{	
	if (!conf.go()->ac.screenFading)
		return; // fading disabled in config
	
	currentFadableWindows.push_back(newScreen); // cache window for later fadeout
	
	if (!screenFadeSurface)
	{
		assert(screen);
		screenFadeSurface = CSDL_Ext::newSurface(screen->w, screen->h);
	}
	SDL_FillRect(screenFadeSurface, nullptr, SDL_MapRGBA(screenFadeSurface->format, 0, 0, 0, 0));
	newScreen->showAll(screenFadeSurface);
	screenFadeAnim->init(CFadeAnimation::EMode::IN, screenFadeSurface, false, 0.1f);
}

void CGuiHandler::fadeOutRemovedScreen(IShowActivatable * removedScreen)
{
	vstd::erase_if_present(currentFadableWindows, removedScreen);
	SDL_FillRect(screenFadeSurface, nullptr, SDL_MapRGBA(screenFadeSurface->format, 0, 0, 0, 0));
	removedScreen->showAll(screenFadeSurface); // blit the removed view on helper surface so it can be faded out
	screenFadeAnim->init(CFadeAnimation::EMode::OUT, screenFadeSurface, false, 0.1f);
}

bool CGuiHandler::canFadeout(const IShowActivatable * removedScreen) const
{
	if (!conf.go()->ac.screenFading)
		return false; // fading disabled in config
	return vstd::contains(currentFadableWindows, removedScreen);
}

void CGuiHandler::updateFade()
{
	if (screenFadeAnim->isFading())
	{
		screenFadeAnim->update();
		fadeFinishedInCurrentFrame = !screenFadeAnim->isFading();
		if (fadeFinishedInCurrentFrame)
			crossfadePush = false;
	}
}

void CGuiHandler::handleElementActivate(CIntObject * elem, ui16 activityFlag)
{
	processLists(activityFlag,[&](std::list<CIntObject*> * lst){
		lst->push_front(elem);
	});
	elem->active_m |= activityFlag;
}

void CGuiHandler::handleElementDeActivate(CIntObject * elem, ui16 activityFlag)
{
	processLists(activityFlag,[&](std::list<CIntObject*> * lst){
		auto hlp = std::find(lst->begin(),lst->end(),elem);
		assert(hlp != lst->end());
		lst->erase(hlp);
	});
	elem->active_m &= ~activityFlag;
}

void CGuiHandler::popInt( IShowActivatable *top )
{
	assert(listInt.front() == top);
	top->deactivate();
	listInt.pop_front();
	objsToBlit -= top;
	if(!listInt.empty())
		listInt.front()->activate();
	totalRedraw();
}

void CGuiHandler::popIntTotally( IShowActivatable *top )
{
	assert(listInt.front() == top);
	popInt(top);
	if (canFadeout(top))
		fadeOutRemovedScreen(top);
	delete top;
	fakeMouseMove();
}

void CGuiHandler::popIntTotallyAndWaitForFadingPush(IShowActivatable * top)
{
	assert(listInt.front() == top);
	SDL_FillRect(screenFadeSurface, nullptr, SDL_MapRGBA(screenFadeSurface->format, 0, 0, 0, 0));
	top->showAll(screenFadeSurface);	
	top->deactivate();
	listInt.pop_front();
	objsToBlit -= top;
	if(!listInt.empty())
		listInt.front()->activate();	
	delete top;
	fakeMouseMove();
	
	crossfadePush = true;
}

void CGuiHandler::pushInt(IShowActivatable *newInt, bool fadein /* = false */)
{
	assert(newInt);
	assert(boost::range::find(listInt, newInt) == listInt.end()); // do not add same object twice
	
	//a new interface will be present, we'll need to use buffer surface (unless it's advmapint that will alter screenBuf on activate anyway)
	screenBuf = screen2;
	
	if (crossfadePush)
	{
		blitAt(screenFadeSurface, 0, 0, screen2);
	}
	
	if (fadein)
		fadeInNewScreen(newInt);

	if(!listInt.empty())
		listInt.front()->deactivate();
	listInt.push_front(newInt);
	newInt->activate();
	objsToBlit.push_back(newInt);
	totalRedraw();
}

void CGuiHandler::popInts( int howMany )
{
	if(!howMany) return; //senseless but who knows...

	assert(listInt.size() >= howMany);
	listInt.front()->deactivate();
	for(int i=0; i < howMany; i++)
	{
		auto front = listInt.front();
		objsToBlit -= front;
		listInt.pop_front();
		if (i + 1 == howMany && canFadeout(front))
			fadeOutRemovedScreen(front);
		delete front;		
	}

	if(!listInt.empty())
	{
		listInt.front()->activate();
		totalRedraw();
	}
	fakeMouseMove();
}

IShowActivatable * CGuiHandler::topInt()
{
	if(listInt.empty())
		return nullptr;
	else
		return listInt.front();
}

void CGuiHandler::updateTime()
{
	int ms = mainFPSmng->getElapsedMilliseconds();
	std::list<CIntObject*> hlp = timeinterested;
	for (auto & elem : hlp)
	{
		if(!vstd::contains(timeinterested,elem)) continue;
		(elem)->onTimer(ms);
	}
}

void CGuiHandler::handleEvents()
{
	//player interface may want special event handling 	
	if(nullptr != LOCPLINT && LOCPLINT->capturedAllEvents())
		return;
	
	boost::unique_lock<boost::mutex> lock(eventsM);	
	while(!events.empty())
	{
		SDL_Event ev = events.front();
		events.pop();		
		this->handleEvent(&ev);
	}
}

void CGuiHandler::handleEvent(SDL_Event *sEvent)
{
	current = sEvent;
	bool prev;

	if (sEvent->type==SDL_KEYDOWN || sEvent->type==SDL_KEYUP)
	{
		SDL_KeyboardEvent key = sEvent->key;

		//translate numpad keys
		if(key.keysym.sym == SDLK_KP_ENTER)
		{
			key.keysym.sym = (SDLKey)SDLK_RETURN;
			#ifndef VCMI_SDL1
			key.keysym.scancode = SDL_SCANCODE_RETURN;
			#endif // VCMI_SDL1
		}

		bool keysCaptured = false;
		for(auto i=keyinterested.begin(); i != keyinterested.end() && current; i++)
		{
			if((*i)->captureThisEvent(key))
			{
				keysCaptured = true;
				break;
			}
		}

		std::list<CIntObject*> miCopy = keyinterested;
		for(auto i=miCopy.begin(); i != miCopy.end() && current; i++)
			if(vstd::contains(keyinterested,*i) && (!keysCaptured || (*i)->captureThisEvent(key)))
				(**i).keyPressed(key);
	}
	else if(sEvent->type==SDL_MOUSEMOTION)
	{
		CCS->curh->cursorMove(sEvent->motion.x, sEvent->motion.y);
		handleMouseMotion(sEvent);
	}
	else if (sEvent->type==SDL_MOUSEBUTTONDOWN)
	{
		if(sEvent->button.button == SDL_BUTTON_LEFT)
		{

			if(lastClick == sEvent->motion  &&  (SDL_GetTicks() - lastClickTime) < 300)
			{
				std::list<CIntObject*> hlp = doubleClickInterested;
				for(auto i=hlp.begin(); i != hlp.end() && current; i++)
				{
					if(!vstd::contains(doubleClickInterested,*i)) continue;
					if (isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y))
					{
						(*i)->onDoubleClick();
					}
				}

			}

			lastClick = sEvent->motion;
			lastClickTime = SDL_GetTicks();

			std::list<CIntObject*> hlp = lclickable;
			for(auto i=hlp.begin(); i != hlp.end() && current; i++)
			{
				if(!vstd::contains(lclickable,*i)) continue;
				if (isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y))
				{
					prev = (*i)->pressedL;
					(*i)->pressedL = true;
					(*i)->clickLeft(true, prev);
				}
			}
		}
		else if (sEvent->button.button == SDL_BUTTON_RIGHT)
		{
			std::list<CIntObject*> hlp = rclickable;
			for(auto i=hlp.begin(); i != hlp.end() && current; i++)
			{
				if(!vstd::contains(rclickable,*i)) continue;
				if (isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y))
				{
					prev = (*i)->pressedR;
					(*i)->pressedR = true;
					(*i)->clickRight(true, prev);
				}
			}
		}
		#ifdef VCMI_SDL1 //SDL1x only events
		else if(sEvent->button.button == SDL_BUTTON_WHEELDOWN || sEvent->button.button == SDL_BUTTON_WHEELUP)
		{
			std::list<CIntObject*> hlp = wheelInterested;
			for(auto i=hlp.begin(); i != hlp.end() && current; i++)
			{
				if(!vstd::contains(wheelInterested,*i)) continue;
				(*i)->wheelScrolled(sEvent->button.button == SDL_BUTTON_WHEELDOWN, isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y));
			}
		}
		#endif
	}
	#ifndef VCMI_SDL1 //SDL2x only events	
	else if (sEvent->type == SDL_MOUSEWHEEL)
	{
		std::list<CIntObject*> hlp = wheelInterested;
		for(auto i=hlp.begin(); i != hlp.end() && current; i++)
		{
			if(!vstd::contains(wheelInterested,*i)) continue;
			(*i)->wheelScrolled(sEvent->wheel.y < 0, isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y));
		}		
	}
	else if(sEvent->type == SDL_TEXTINPUT)
	{
		for(auto it : textInterested)
		{
			it->textInputed(sEvent->text);
		}
	}	
	else if(sEvent->type == SDL_TEXTEDITING)
	{
		for(auto it : textInterested)
		{
			it->textEdited(sEvent->edit);
		}
	}	
	//todo: muiltitouch
	#endif // VCMI_SDL1
	else if ((sEvent->type==SDL_MOUSEBUTTONUP) && (sEvent->button.button == SDL_BUTTON_LEFT))
	{
		std::list<CIntObject*> hlp = lclickable;
		for(auto i=hlp.begin(); i != hlp.end() && current; i++)
		{
			if(!vstd::contains(lclickable,*i)) continue;
			prev = (*i)->pressedL;
			(*i)->pressedL = false;
			if (isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y))
			{
				(*i)->clickLeft(false, prev);
			}
			else
				(*i)->clickLeft(boost::logic::indeterminate, prev);
		}
	}
	else if ((sEvent->type==SDL_MOUSEBUTTONUP) && (sEvent->button.button == SDL_BUTTON_RIGHT))
	{
		std::list<CIntObject*> hlp = rclickable;
		for(auto i=hlp.begin(); i != hlp.end() && current; i++)
		{
			if(!vstd::contains(rclickable,*i)) continue;
			prev = (*i)->pressedR;
			(*i)->pressedR = false;
			if (isItIn(&(*i)->pos,sEvent->motion.x,sEvent->motion.y))
			{
				(*i)->clickRight(false, prev);
			}
			else
				(*i)->clickRight(boost::logic::indeterminate, prev);
		}
	}
	current = nullptr;

} //event end

void CGuiHandler::handleMouseMotion(SDL_Event *sEvent)
{
	//sending active, hovered hoverable objects hover() call
	std::vector<CIntObject*> hlp;
	for(auto & elem : hoverable)
	{
		if (isItIn(&(elem)->pos,sEvent->motion.x,sEvent->motion.y))
		{
			if (!(elem)->hovered)
				hlp.push_back((elem));
		}
		else if ((elem)->hovered)
		{
			(elem)->hover(false);
			(elem)->hovered = false;
		}
	}
	for(auto & elem : hlp)
	{
		elem->hover(true);
		elem->hovered = true;
	}

	handleMoveInterested(sEvent->motion);
}

void CGuiHandler::simpleRedraw()
{	
	if (fadeFinishedInCurrentFrame && !crossfadePush)
	{
		fadeFinishedInCurrentFrame = false;
		totalRedraw(); // needs full redraw after fading, because new window wasn't drawn on screen2 before
	}
	else
	{
		//update only top interface and draw background
		if(objsToBlit.size() > 1 || screenFadeAnim->isFading())
			blitAt(screen2,0,0,screen); //blit background
		
		if (!screenFadeAnim->isFading())			
			objsToBlit.back()->show(screen); //blit active interface/window
		else
			screenFadeAnim->draw(screen, nullptr, nullptr);	
	}
}

void CGuiHandler::totalRedraw()
{		
	if (screenFadeAnim->isFading() && crossfadePush) // don't touch screen2 during crossfade
		simpleRedraw();
	else
	{
		for(auto & elem : objsToBlit)
			if (!screenFadeAnim->isFading() || elem != objsToBlit.back() || screenFadeAnim->fadingMode != CFadeAnimation::EMode::IN)
				elem->showAll(screen2);
	
		blitAt(screen2,0,0,screen);
		if (screenFadeAnim->isFading())
			screenFadeAnim->draw(screen, nullptr, nullptr);				
	}
}

void CGuiHandler::handleMoveInterested( const SDL_MouseMotionEvent & motion )
{
	//sending active, MotionInterested objects mouseMoved() call
	std::list<CIntObject*> miCopy = motioninterested;
	for(auto & elem : miCopy)
	{
		if ((elem)->strongInterest || isItIn(&(elem)->pos, motion.x, motion.y))
		{
			(elem)->mouseMoved(motion);
		}
	}
}

void CGuiHandler::fakeMouseMove()
{
	SDL_Event evnt;
#ifdef VCMI_SDL1
	SDL_MouseMotionEvent sme = {SDL_MOUSEMOTION, 0, 0, 0, 0, 0, 0};
#else
	SDL_MouseMotionEvent sme = {SDL_MOUSEMOTION, 0, 0, 0, 0, 0, 0, 0, 0};
#endif	
	int x, y;
	sme.state = SDL_GetMouseState(&x, &y);
	sme.x = x;
	sme.y = y;

	evnt.motion = sme;
	current = &evnt;
	handleMouseMotion(&evnt);
}

void CGuiHandler::renderFrame()
{	
	auto doUpdate = [this]()
	{
		updateFade();
		if(nullptr != curInt)
		{
			curInt -> update();
		}			
		// draw the mouse cursor and update the screen
		CCS->curh->render();

		#ifndef	VCMI_SDL1
		if(0 != SDL_RenderCopy(mainRenderer, screenTexture, nullptr, nullptr))
			logGlobal->errorStream() << __FUNCTION__ << " SDL_RenderCopy " << SDL_GetError();

		SDL_RenderPresent(mainRenderer);				
		#endif		
		
	};
	
	if(curInt)
		curInt->runLocked(doUpdate);
	else
		doUpdate();
	
	mainFPSmng->framerateDelay(); // holds a constant FPS	
}


CGuiHandler::CGuiHandler()
	: screenFadeAnim(new CFadeAnimation()), 
	  screenFadeSurface(nullptr),
	  fadeFinishedInCurrentFrame(false),
	  crossfadePush(false),
	  lastClick(-500, -500)
{
	curInt = nullptr;
	current = nullptr;
	statusbar = nullptr;

	// Creates the FPS manager and sets the framerate to 48 which is doubled the value of the original Heroes 3 FPS rate
	mainFPSmng = new CFramerateManager(48);
	//do not init CFramerateManager here --AVS
}

CGuiHandler::~CGuiHandler()
{
	delete mainFPSmng;
	delete screenFadeAnim;
	if (screenFadeSurface)
		SDL_FreeSurface(screenFadeSurface);
}

void CGuiHandler::breakEventHandling()
{
	current = nullptr;
}

void CGuiHandler::drawFPSCounter()
{
	const static SDL_Color yellow = {255, 255, 0, 0};
	static SDL_Rect overlay = { 0, 0, 64, 32};
	Uint32 black = SDL_MapRGB(screen->format, 10, 10, 10);
	SDL_FillRect(screen, &overlay, black);
	std::string fps = boost::lexical_cast<std::string>(mainFPSmng->fps);
	graphics->fonts[FONT_BIG]->renderTextLeft(screen, fps, yellow, Point(10, 10));
}

SDLKey CGuiHandler::arrowToNum( SDLKey key )
{
	#ifdef VCMI_SDL1
	switch(key)
	{
	case SDLK_DOWN:
		return SDLK_KP2;
	case SDLK_UP:
		return SDLK_KP8;
	case SDLK_LEFT:
		return SDLK_KP4;
	case SDLK_RIGHT:
		return SDLK_KP6;
	default:
		throw std::runtime_error("Wrong key!");assert(0);
	}	
	#else
	switch(key)
	{
	case SDLK_DOWN:
		return SDLK_KP_2;
	case SDLK_UP:
		return SDLK_KP_8;
	case SDLK_LEFT:
		return SDLK_KP_4;
	case SDLK_RIGHT:
		return SDLK_KP_6;
	default:
		throw std::runtime_error("Wrong key!");
	}	
	#endif // 0
}

SDLKey CGuiHandler::numToDigit( SDLKey key )
{
#ifdef VCMI_SDL1
	if(key >= SDLK_KP0 && key <= SDLK_KP9)
		return SDLKey(key - SDLK_KP0 + SDLK_0);
#endif // 0

#define REMOVE_KP(keyName) case SDLK_KP_ ## keyName : return SDLK_ ## keyName;
	switch(key)
	{
#ifndef VCMI_SDL1
		REMOVE_KP(0)
		REMOVE_KP(1)
		REMOVE_KP(2)
		REMOVE_KP(3)
		REMOVE_KP(4)
		REMOVE_KP(5)
		REMOVE_KP(6)
		REMOVE_KP(7)
		REMOVE_KP(8)
		REMOVE_KP(9)		
#endif // VCMI_SDL1		
		REMOVE_KP(PERIOD)
		REMOVE_KP(MINUS)
		REMOVE_KP(PLUS)
		REMOVE_KP(EQUALS)

	case SDLK_KP_MULTIPLY:
		return SDLK_ASTERISK;
	case SDLK_KP_DIVIDE:
		return SDLK_SLASH;
	case SDLK_KP_ENTER:
		return SDLK_RETURN;
	default:
		return SDLK_UNKNOWN;
	}
#undef REMOVE_KP
}

bool CGuiHandler::isNumKey( SDLKey key, bool number )
{
	#ifdef VCMI_SDL1
	if(number)
		return key >= SDLK_KP0 && key <= SDLK_KP9;
	else
		return key >= SDLK_KP0 && key <= SDLK_KP_EQUALS;
	#else
	if(number)
		return key >= SDLK_KP_1 && key <= SDLK_KP_0;
	else
		return (key >= SDLK_KP_1 && key <= SDLK_KP_0) || key == SDLK_KP_MINUS || key == SDLK_KP_PLUS || key == SDLK_KP_EQUALS;
	#endif // 0
}

bool CGuiHandler::isArrowKey( SDLKey key )
{
	return key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT;
}

bool CGuiHandler::amIGuiThread()
{
	return inGuiThread.get() && *inGuiThread;
}

void CGuiHandler::pushSDLEvent(int type, int usercode)
{
	SDL_Event event;
	event.type = type;
	event.user.code = usercode;	// not necessarily used
	SDL_PushEvent(&event);
}

CFramerateManager::CFramerateManager(int rate)
{
	this->rate = rate;
	this->rateticks = (1000.0 / rate);
	this->fps = 0;
}

void CFramerateManager::init()
{
	this->lastticks = SDL_GetTicks();
}

void CFramerateManager::framerateDelay()
{
	ui32 currentTicks = SDL_GetTicks();
	timeElapsed = currentTicks - lastticks;

	// FPS is higher than it should be, then wait some time
	if (timeElapsed < rateticks)
	{
		SDL_Delay(ceil(this->rateticks) - timeElapsed);
	}
	currentTicks = SDL_GetTicks();

	fps = ceil(1000.0 / timeElapsed);

	// recalculate timeElapsed for external calls via getElapsed()
	// limit it to 1000 ms to avoid breaking animation in case of huge lag (e.g. triggered breakpoint)
	timeElapsed = std::min<ui32>(currentTicks - lastticks, 1000);
	lastticks = SDL_GetTicks();
}
