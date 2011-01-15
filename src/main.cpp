#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <fstream>
#include <GL/gl.h>
#include <GL/glu.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <jack/jack.h>
#include <flux.h>

using namespace std;

double gTime, startTime;


double getTime()
{
	timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + tv.tv_usec*0.000001;
}


inline bool checkglerror(bool fatal= false)
{
    int err= glGetError();
    if(err)
    {
    	printf("GL Error: %s\n", gluErrorString(err));
        if(fatal) abort();
        else return false;
    }
    return true;
}


void setVideoMode(int w, int h)
{
	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 0);
	SDL_SetVideoMode(w, h, 0, SDL_OPENGL|SDL_RESIZABLE);
	int doubleBuf, depthSize, swapControl;
	SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &doubleBuf);
	SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthSize);
	SDL_GL_GetAttribute(SDL_GL_SWAP_CONTROL, &swapControl);
	SDL_WM_SetCaption("Scope", 0);
	SDL_ShowCursor(false);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
    glOrtho(0, w, h, 0, -1000, 1000);
    glViewport(0,0, w,h);
	glDisable(GL_DITHER);
	flux_screenresize(w, h);
}


void flux_tick()
{
	aq_exec();
	run_timers();

	redraw_rect(&viewport);
	redraw_cursor();

	checkglerror();
}

int SDLMouseButtonToFluxMouseButton(int button)
{
	if(button>3) return button-1;
	else return (button==1? 0: button==3? 1: 2);
}

class SDLScopedLock
{
	public:
		SDLScopedLock(SDL_mutex *m): myMutex(m)
		{
			SDL_LockMutex(myMutex);
		}

		~SDLScopedLock()
		{
			SDL_UnlockMutex(myMutex);
		}

	private:
		SDL_mutex *myMutex;
};

enum SDLUserEvent
{
	SDL_USER_ADDJACKBUFFER= 0,
};


class configHandler
{
	private:
		vector<class configOptionHandler *> configOptionHandlers;

	public:
		void addConfigOptionHandler(class configOptionHandler *cs)
		{ configOptionHandlers.push_back(cs); }

		void removeConfigOptionHandler(class configOptionHandler *cs)
		{
			vector<class configOptionHandler*>::iterator it=
				find(configOptionHandlers.begin(), configOptionHandlers.end(), cs);
			if(it!=configOptionHandlers.end()) configOptionHandlers.erase(it);
		}

		bool writeToFile(const char *filename);
		bool readFromFile(const char *filename);
};

configHandler gConfigHandler;

class configOptionHandler
{
	private:
		enum optionType
		{
			OT_FLOAT= 0,
			OT_BOOL
		};
		struct configOption
		{
			optionType type;
			std::string name;
			void *address;

			std::string getString()
			{
				std::ostringstream s;
				switch(type)
				{
					case OT_FLOAT:
						s << *(float*)address;
						return s.str();
					case OT_BOOL:
						s << *(bool*)address;
						return s.str();
					default:
						return std::string("unknown option type!");
				}
			}

			void putString(const std::string &str)
			{
				std::istringstream s(str);
				switch(type)
				{
					case OT_FLOAT:
						s >> *(float*)address;
						break;
					case OT_BOOL:
						s >> *(bool*)address;
						break;
					default:
						puts("unknown option type!");
				}
			}
		};
		vector<configOption> configOptions;
		std::string name;

		void addConfigOption(const char *name, optionType type, void *address)
		{ configOptions.push_back( (configOption) { type, std::string(name), address } ); }

	protected:
		configOptionHandler(const char *_name):
			name(_name)
		{ gConfigHandler.addConfigOptionHandler(this); }

		virtual ~configOptionHandler()
		{ gConfigHandler.removeConfigOptionHandler(this); }

		void addConfigOption(const char *name, float *address)
		{ addConfigOption(name, OT_FLOAT, address); }

		void addConfigOption(const char *name, bool *address)
		{ addConfigOption(name, OT_BOOL, address); }

		#define ADD_CONFIG_OPTION(var) addConfigOption(#var, &var)

	public:
		const std::string &getName()
		{ return name; }

		std::string writeToString()
		{
			std::string ret;
			for(vector<configOption>::iterator i= configOptions.begin(); i!=configOptions.end(); i++)
				ret.append(name + "." + i->name + "=" + i->getString() + "\n");
			return ret;
		}

		bool writeToFile(const char *filename)
		{
			FILE *f= fopen(filename, "a");
			if(!f) return false;
			std::string s= writeToString();
			if(fwrite(s.c_str(), 1, s.length(), f)!=s.length())
			{ fclose(f); return false; }
			fclose(f);
			return true;
		}

		bool readItem(const char *itemName, const char *itemValue)
		{
			for(vector<configOption>::iterator i= configOptions.begin(); i!=configOptions.end(); i++)
			{
				if(i->name==itemName)
				{
					i->putString(itemValue);
					return true;
				}
			}
			return false;
		}

		static bool parseLine(char *line, char *&name, char *&itemName, char *&itemValue)
		{
			name= line;
			while(isspace(*name)) name++;
			char *s= name; while(*s && !isspace(*s) && *s!='.') s++;
			if(!*s) { *name= 0; itemName= itemValue= name; return true; }	// empty line
			*s++= 0; while(*s && isspace(*s)) s++;
			if(!*s) return false;
			itemName= s;
			while(*s && !isspace(*s) && *s!='=') s++;
			if(!*s) return false;
			*s++= 0; while(*s && isspace(*s)) s++;
			if(!*s) return false;
			itemValue= s;
			return true;
		}

		bool readFromString(std::string src)
		{
			char line[1024];
			int i= 1;
			for(std::stringstream stream(src); !stream.eof(); i++)
			{
				stream.getline(line, sizeof(line));
				char *name, *itemName, *itemValue;
				if(!parseLine(line, name, itemName, itemValue))
					printf("parse error at line %d\n", i);
				else if(name==this->name)
					readItem(itemName, itemValue);
			}
			return true;
		}

		bool readFromFile(const char *filename)
		{
			ifstream f;
			f.open(filename, ios_base::in);
			if(f.fail()) return false;
			stringbuf sbuf;
			f.get(sbuf, 0);
			string str;
			readFromString(sbuf.str());
			return true;
		}
};


bool configHandler::writeToFile(const char *filename)
{
	bool ok= true;
	for(vector<configOptionHandler*>::iterator it= configOptionHandlers.begin(); it!=configOptionHandlers.end(); it++)
		if(!(*it)->writeToFile(filename)) ok= false;
	return ok;
}

bool configHandler::readFromFile(const char *filename)
{
	char *name, *itemName, *itemValue;
	std::string lastName;
	configOptionHandler *handler;
	char line[1024];

	ifstream f;
	f.open(filename, ios_base::in);
	if(f.fail()) return false;
	while(!f.eof())
	{
		f.getline(line, 1023);
		if(f.fail()) return false;
		if(configOptionHandler::parseLine(line, name, itemName, itemValue))
		{
			if(lastName!=name)
			{
				handler= 0;
				for(int i= 0; i<configOptionHandlers.size(); i++)
				{
					if(configOptionHandlers[i]->getName()==name)
					{
						handler= configOptionHandlers[i];
						lastName= name;
						break;
					}
				}
				if(!handler) continue;
			}
			handler->readItem(itemName, itemValue);
		}
	}
	return true;
}

struct JackBufferData
{
	jack_default_audio_sample_t **data;
	int nFrames;
	int nChannels;

	JackBufferData(int nChannels, int nFrames)
	{
		data= new jack_default_audio_sample_t* [nChannels];
		for(int i= 0; i<nChannels; i++)
			data[i]= new jack_default_audio_sample_t[nFrames];
		this->nChannels= nChannels;
		this->nFrames= nFrames;
	}
	~JackBufferData()
	{
		for(int i= 0; i<nChannels; i++)
			delete[] data[i];
		delete[] data;
	}
};

class JackInterface
{
	public:
		JackInterface(): client(0), running(false), eventData(0)
		{
		}

		~JackInterface()
		{
			if(running) jack_client_close(client);
			if(eventData) delete(eventData);
		}

		bool initialize(int nChannels)
		{
			const char *client_name = "fluxscope";
			const char *server_name = NULL;
			jack_options_t options = JackNoStartServer;
			jack_status_t status;

			client= jack_client_open (client_name, options, &status, server_name);
			if (client == NULL) {
				fprintf (stderr, "jack_client_open() failed, "
					 "status = 0x%2.0x\n", status);
				if (status & JackServerFailed) {
					fprintf (stderr, "Unable to connect to JACK server\n");
				}
				return false;
			}
			if (status & JackServerStarted) {
				fprintf (stderr, "JACK server started\n");
			}
			if (status & JackNameNotUnique) {
				client_name = jack_get_client_name(client);
				fprintf (stderr, "unique name `%s' assigned\n", client_name);
			}

			/* tell the JACK server to call `process()' whenever
			   there is work to be done.
			*/
			jack_set_process_callback(client, jackProcess, this);

			/* tell the JACK server to call `jack_shutdown()' if
			   it ever shuts down, either entirely, or if it
			   just decides to stop calling us.
			*/
			jack_on_info_shutdown (client, jackInfoShutdownCB, this);

			/* display the current sample rate.
			 */
			printf("engine sample rate: %d\n", jack_get_sample_rate(client));

			/* create ports */
			inputPorts.reserve(nChannels);
			for(int i= 0; i<nChannels; i++)
			{
				char portName[64];
				snprintf(portName, 64, "input%02d", i);
				jack_port_t *inputPort= jack_port_register (client, portName,
															JACK_DEFAULT_AUDIO_TYPE,
															JackPortIsInput, 0);
				if ((inputPort == NULL)) {
					fprintf(stderr, "no more JACK ports available\n");
					return false;
				}
				inputPorts.push_back(inputPort);
			}

			// allocate event data structure
			if(eventData) delete(eventData), eventData= 0;
			eventData= new JackBufferData(nChannels, jack_get_buffer_size(client));

			/* Tell the JACK server that we are ready to roll.  Our
			 * process() callback will start running now. */
			if (jack_activate (client)) {
				fprintf (stderr, "cannot activate client");
				return false;
			}

			return (running= true);
		}

		int getSamplingRate()
		{
			if(running)
				return jack_get_sample_rate(client);
			else
				return 48000;
		}

		bool isRunning()
		{
			return running;
		}

	private:
		vector<jack_port_t *> inputPorts;
		jack_client_t *client;
		JackBufferData *eventData;
		bool running;

		int process(jack_nframes_t nframes)
		{
			for(int i= 0; i<inputPorts.size(); i++)
			{
				memcpy(eventData->data[i],
					   jack_port_get_buffer(inputPorts[i], nframes),
					   nframes*sizeof(jack_default_audio_sample_t));
			}

			SDL_UserEvent event;
			event.type= SDL_USEREVENT;
			event.code= SDL_USER_ADDJACKBUFFER;
			event.data1= (void*)eventData;
			SDL_PushEvent((SDL_Event*)&event);

			return 0;
		}

		static int jackProcess(jack_nframes_t nframes, void *arg)
		{
			return reinterpret_cast<JackInterface*>(arg)->process(nframes);
		}

		void jackInfoShutdown(jack_status_t code, const char *reason)
		{
			printf("JACK shutdown: %s\n", reason);
			running= false;
		}

		static void jackInfoShutdownCB(jack_status_t code, const char *reason, void *arg)
		{
			reinterpret_cast<JackInterface*>(arg)->jackInfoShutdown(code, reason);
		}
};


class fluxWindowBase
{
	private:
		static void basePaintCB(prop_t arg, struct primitive *self, rect *abspos, const rectlist *dirty_rects)
		{
			reinterpret_cast<fluxWindowBase*>(arg)->cbPaint(self, abspos, dirty_rects);
		}

		static int baseMouseCB(prop_t arg, struct primitive *self, int type, int x, int y, int btn)
		{
			reinterpret_cast<fluxWindowBase*>(arg)->cbMouse(self, type, x, y, btn);
			return 1;
		}

	protected:
		uint32_t fluxHandle;

		enum
		{
//			CB_KEYBD_FLAG= (1<<CB_KEYBD),
			CB_MOUSE_FLAG= (1<<primitive::CB_MOUSE),
			CB_PAINT_FLAG= (1<<primitive::CB_PAINT),
//			CB_PROPS_FLAG= (1<<CB_PROPS),
//			CB_STATUS_FLAG= (1<<CB_STATUS)
		};

		fluxWindowBase(int x, int y, int w, int h, uint32_t callbackFlags, int parent= NOPARENT, int alignment= ALIGN_LEFT|ALIGN_TOP)
		{
			fluxHandle= create_rect(parent, x,y, w,h, COL_WINDOW, alignment);
			if(callbackFlags&CB_PAINT_FLAG) wnd_set_paint_callback(fluxHandle, basePaintCB, reinterpret_cast<prop_t>(this));
			if(callbackFlags&CB_MOUSE_FLAG) wnd_set_mouse_callback(fluxHandle, baseMouseCB, reinterpret_cast<prop_t>(this));
			wnd_setresizable(fluxHandle, false);
		}

		virtual ~fluxWindowBase()
		{
			if(fluxHandle!=NOWND) wnd_destroy(fluxHandle);
			fluxHandle= NOWND;
		}

		virtual void cbPaint(primitive *self, rect *abspos, const rectlist *dirty_rects) { }
		virtual void cbMouse(primitive *self, int type, int x, int y, int btn) { }
};

class fluxOscWindow: public fluxWindowBase, public configOptionHandler
{
	public:
		fluxOscWindow(int x, int y, int w, int h, int parent= NOPARENT, int alignment= ALIGN_LEFT|ALIGN_TOP):
			fluxWindowBase(x,y, w,h, CB_MOUSE_FLAG|CB_PAINT_FLAG, parent, alignment),
			configOptionHandler("OscWindow"),
			sampleBufferFillIndex(0), triggerLevel(0.2), triggerEnabled(true), triggerPositive(true),
			verticalScaling(1.0), samplingRate(48000), draggingHorizScale(false), configPane(false)
		{
			setDisplayTime(0.01);

			ADD_CONFIG_OPTION(displayTime);
			ADD_CONFIG_OPTION(triggerLevel);
			ADD_CONFIG_OPTION(triggerPositive);
			ADD_CONFIG_OPTION(triggerEnabled);
			ADD_CONFIG_OPTION(verticalScaling);
		}

		~fluxOscWindow()
		{
		}

		void setConfigPane(class fluxOscWindowConfigPane *myConfigPane)
		{
			configPane= myConfigPane;
		}

		void addBuffer(jack_default_audio_sample_t *data, uint32_t nFrames, int channelNr)
		{
			bool needRepaint= (nFrames<sampleBuffer.size()? true: false);
			if(!triggerEnabled)
			{
				if(sampleBufferFillIndex>=sampleBuffer.size()) sampleBufferFillIndex= 0;
				int srcIndex= 0;
				while(nFrames)
				{
					int framesToCopy= min(nFrames, sampleBuffer.size()-sampleBufferFillIndex);
					memcpy(&sampleBuffer[sampleBufferFillIndex], data+srcIndex, framesToCopy*sizeof(jack_default_audio_sample_t));
					nFrames-= framesToCopy;
					sampleBufferFillIndex+= framesToCopy;
					srcIndex+= framesToCopy;
					if(sampleBufferFillIndex==sampleBuffer.size())
						sampleBufferFillIndex= 0,
						needRepaint= true;
				}
			}
			else
			{
				for(uint32_t i= 0; i<nFrames; i++)
				{
					jack_default_audio_sample_t sample= data[i];
					if(sampleBufferFillIndex<sampleBuffer.size())
					{
						sampleBuffer[sampleBufferFillIndex++]= sample;
						if(sampleBufferFillIndex==sampleBuffer.size())
							needRepaint= true;
					}
					else if( (triggerPositive && (prevTriggerSample<=triggerLevel && sample>=triggerLevel)) ||
							 ((!triggerPositive) && (prevTriggerSample>=triggerLevel && sample<=triggerLevel)) )
					{
						sampleBufferFillIndex= 0;
						needRepaint= true;
					}
					prevTriggerSample= sample;
				}
			}

			if(needRepaint)
				doRepaint();
		}

		void setDisplayTime(double time)
		{ setDisplaySamples(int(time*samplingRate)); }

		double getDisplayTime()
		{ return displayTime; }

		void setDisplaySamples(int nFrames)
		{
			if(nFrames<10) nFrames= 10;
			else if(nFrames>samplingRate*10) nFrames= samplingRate*10;
			sampleBuffer.resize(nFrames);
			displayTime= double(sampleBuffer.size())/samplingRate;
			doRepaint();
		}

		void enableTrigger(bool enabled)
		{ triggerEnabled= enabled; }

		void setTriggerLevel(float level)
		{ triggerLevel= level; }

		void setTriggerDir(bool positive)
		{ triggerPositive= positive; }

		float getVerticalScaling()
		{ return verticalScaling; }

		void setVerticalScaling(float s)
		{
			verticalScaling= (s<0.1? 0.1: s>100? 100: s);
			float triggerMinMax= 1.0/verticalScaling;
			if(triggerLevel>triggerMinMax) triggerLevel= triggerMinMax;
			if(triggerLevel<-triggerMinMax) triggerLevel= -triggerMinMax;
			updateGuiParam(&triggerLevel);
		}

		void setSamplingRate(float s)
		{ samplingRate= s; }

		bool isTriggerEnabled()
		{ return triggerEnabled; }

		bool isTriggerPositive()
		{ return triggerPositive; }

		float getTriggerLevel()
		{ return triggerLevel; }

	private:
		vector<jack_default_audio_sample_t> sampleBuffer;
		uint32_t sampleBufferFillIndex;
		struct gl2DCoords { float x; float y; };
		vector<gl2DCoords> glCoords;
		float triggerLevel;
		bool triggerEnabled;
		bool triggerPositive;
		jack_default_audio_sample_t prevTriggerSample;
		float verticalScaling;
		float samplingRate;
		float displayTime;
		bool draggingHorizScale;
		int horizScaleClickPos;
		int cursorPos;
		class fluxOscWindowConfigPane *configPane;

		double getValueAtSamplePos(double pos)
		{
			uint32_t idx0= uint32_t(pos), idx1= (idx0<sampleBuffer.size()-1? idx0+1: sampleBuffer.size()-1);
			double a= pos-idx0;
			return sampleBuffer[idx0] + (sampleBuffer[idx1]-sampleBuffer[idx0])*a;
		}

		void updateGuiParam(void *paramAddress);

		void doRepaint()
		{
			rect absPos;
			wnd_get_abspos(fluxHandle, &absPos);
			uint32_t windowWidth= absPos.rgt - absPos.x;
			if(glCoords.size() != windowWidth)
				glCoords.resize(windowWidth);
			if(!windowWidth) return;

			double sampleStep= double(sampleBuffer.size())/glCoords.size();
			int coordIdx= 0;
			for(double sampleIdx= 0; sampleIdx<(int)sampleBuffer.size() && coordIdx<(int)windowWidth;
				sampleIdx+= sampleStep, coordIdx++)
			{
				glCoords[coordIdx].x= coordIdx;
				glCoords[coordIdx].y= getValueAtSamplePos(sampleIdx);
			}
		}

		void cbPaint(primitive *self, rect *absPos, const rectlist *dirtyRects)
		{
			uint32_t windowWidth= absPos->rgt - absPos->x;
			int windowHeight= absPos->btm - absPos->y;
			if(glCoords.size() != windowWidth)
				doRepaint();

			if(!windowWidth) return;

			fill_rect(absPos, 0);

			glEnable(GL_SCISSOR_TEST);
			glEnable(GL_BLEND);
			glDisable(GL_LINE_SMOOTH);

			glScissor(absPos->x, viewport.btm-absPos->btm, windowWidth, windowHeight);

			glMatrixMode(GL_MODELVIEW);
			glPushMatrix();
			glTranslatef(absPos->x, absPos->y + windowHeight*.5, 0);
			glScalef(1, -(windowHeight)*.5, 1);

			glBlendFunc(GL_SRC_ALPHA, GL_ONE);

			glBegin(GL_LINES);

			// draw segments
			int STEPS= 16;
			for(int i= 1; i<STEPS; i++)
			{
				float y= i*2.0/STEPS-1;
				if(!(i&1)) glColor4f(1,1,1,.3);
				else glColor4f(1,1,1,.2);

				glVertex2f(0, y);
				glVertex2f(windowWidth, y);

				float x= i*windowWidth/STEPS;
				glVertex2f(x, -1);
				glVertex2f(x, +1);
			}

			// draw center line
			glColor4f(1,1,1,.5);
			glVertex2f(0, 0);
			glVertex2f(windowWidth, 0);

			glEnd();

			// draw cursor
			if(cursorPos>=0 && cursorPos<(int)windowWidth)
			{
				glColor4f(1,.6,.2, .5);
				glBegin(GL_LINES);
				glVertex2f(cursorPos, -1);
				glVertex2f(cursorPos, 1);
				glEnd();

				double windowPos= double(cursorPos)/windowWidth;
				double valueAtCursor= getValueAtSamplePos(windowPos*sampleBuffer.size())*verticalScaling;
				glEnable(GL_LINE_SMOOTH);
				glBegin(GL_LINES);
				glVertex2f(cursorPos-4, valueAtCursor-0.035);
				glVertex2f(cursorPos+4, valueAtCursor+0.035);
				glVertex2f(cursorPos+4, valueAtCursor-0.035);
				glVertex2f(cursorPos-4, valueAtCursor+0.035);
				glEnd();
				glDisable(GL_LINE_SMOOTH);
			}

			glPopMatrix();
			glPushMatrix();
			glTranslatef(absPos->x, absPos->y + (absPos->btm-absPos->y)/2+.5, 0);
			glScalef(1, -(absPos->btm-absPos->y-1)/2*verticalScaling, 1);

			if(triggerEnabled)
			{
				// draw trigger
				glBegin(GL_LINES);
				glColor4f(0,1,1,.5);
				glVertex2f(0, triggerLevel);
				glVertex2f(windowWidth, triggerLevel);
				glEnd();
			}

			glColor4f(.1,1,.25,.75);
			glEnable(GL_LINE_SMOOTH);
			glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
			glLineWidth(1.5);
			glVertexPointer(2, GL_FLOAT, 0, (void*)&glCoords[0]);
			glEnable(GL_VERTEX_ARRAY);
			glDrawArrays(GL_LINE_STRIP, 0, windowWidth);
			glDisable(GL_VERTEX_ARRAY);
			glPopMatrix();
			glLineWidth(1);

			if(cursorPos>=0 && cursorPos<absPos->rgt-absPos->x)
			{
				char cursorText[128];
				double windowPos= double(cursorPos)/windowWidth;
				double valueAtCursor= sampleBuffer[ int(windowPos*sampleBuffer.size()) ];
				double timeIdx= windowPos*sampleBuffer.size()/samplingRate;
				snprintf(cursorText, 128, "+%.2fms Value: %7.4f", timeIdx*1000, valueAtCursor);
				draw_text(_font_getloc(FONT_DEFAULT), cursorText, 4,absPos->btm-4-13, *absPos, 0x10f008);
			}

			glDisable(GL_SCISSOR_TEST);
		}

		void cbMouse(primitive *self, int type, int x, int y, int btn)
		{
			if(type==MOUSE_DOWN && btn==MOUSE_BTNWHEELUP)
				setVerticalScaling(verticalScaling*1.1),
				updateGuiParam(&verticalScaling);
			else if(type==MOUSE_DOWN && btn==MOUSE_BTNWHEELDOWN)
				setVerticalScaling(verticalScaling*0.9),
				updateGuiParam(&verticalScaling);
			else if(type==MOUSE_DOWN && btn==MOUSE_BTNRIGHT)
			{
				draggingHorizScale= true;
				horizScaleClickPos= x;
				cursorPos= -1;
			}
			else if( btn==1 && triggerEnabled )
			{
				if(type==MOUSE_DOWN)
					wnd_set_mouse_capture(fluxHandle);
				rect absPos;
				wnd_get_abspos(fluxHandle, &absPos);
				int windowHeight= absPos.btm-absPos.y;
				triggerLevel= double(windowHeight/2-y)/verticalScaling/(absPos.btm-absPos.y)*2;
				float triggerMinMax= 1.0/verticalScaling;
				if(triggerLevel>triggerMinMax) triggerLevel= triggerMinMax;
				if(triggerLevel<-triggerMinMax) triggerLevel= -triggerMinMax;
				updateGuiParam(&triggerLevel);
				cursorPos= -1;
			}
			else if(type==MOUSE_UP)
			{
				draggingHorizScale= false;
				wnd_set_mouse_capture(NOWND);
			}
			else if(type==MOUSE_OVER && draggingHorizScale)
			{
				setDisplayTime(displayTime+(horizScaleClickPos-x)*0.0001);
				updateGuiParam(&displayTime);
				horizScaleClickPos= x;
			}
			else if(type==MOUSE_OVER)
			{
				cursorPos= x;
			}
			else if(type==MOUSE_OUT)
			{
				cursorPos= -1;
			}
		}
};

// receives callbacks when some kind of object has changed
class changeListener
{
	public:
		// this is called when some object has changed.
		// override this in the subclass
		virtual void valueChanged(class changeNotifier *which)
		{ }
};

// calls back a changeListener when it has changed
class changeNotifier
{
	private:
		changeListener *myChangeListener;

	public:
		changeNotifier(changeListener *_myChangeListener):
			myChangeListener(_myChangeListener)
		{ }

		// call this to notify the changeListener that this object has changed
		void notifyChange()
		{
			if(myChangeListener) myChangeListener->valueChanged(this);
		}
};

// base class for a label that can be changed in some way.
// it just creates the text window and underlines it when the mouse enters the window.
class fluxChangableLabelBase: public fluxWindowBase, public changeNotifier
{
	protected:
		uint32_t labelHandle, lineHandle;

		void cbMouse(primitive *self, int type, int x, int y, int btn)
		{
			if(type==MOUSE_IN)
				wnd_show(lineHandle, true);
			else if(type==MOUSE_OUT)
				wnd_show(lineHandle, false);
		}


	public:
		fluxChangableLabelBase(changeListener *_myChangeListener,
							   int x, int y, uint32_t callbackFlags,
							   int parent= NOPARENT, int alignment= ALIGN_LEFT|ALIGN_TOP):
			fluxWindowBase(x,y, 0,0, callbackFlags|CB_MOUSE_FLAG, parent, alignment),
			changeNotifier(_myChangeListener)
		{
			labelHandle= create_text(fluxHandle, 0,0, 0,0, "text", COL_TEXT, FONT_DEFAULT, ALIGN_LEFT|ALIGN_RIGHT|ALIGN_TOP|ALIGN_BOTTOM);
			lineHandle= create_rect(fluxHandle, 0,0, 0,1, COL_TEXT, ALIGN_LEFT|ALIGN_RIGHT|ALIGN_BOTTOM);
			wnd_show(lineHandle, false);
			rect_setcolor(fluxHandle, TRANSL_NOPAINT);
			setText("Label");
		}

		~fluxChangableLabelBase()
		{
			wnd_destroy(labelHandle);
			wnd_destroy(lineHandle);
		}

		void setText(const char *text)
		{
			text_settext(labelHandle, text);
			wnd_setsize(fluxHandle, font_gettextwidth(FONT_DEFAULT, text), font_gettextheight(FONT_DEFAULT, text));
		}
};

class fluxDraggableLabel: public fluxChangableLabelBase
{
	public:
		enum DisplayMode
		{
			DM_PLAIN= 0,
			DM_SECONDS,
			DM_PERCENTAGE
		};

		fluxDraggableLabel(changeListener *_myChangeListener, int x, int y, int parent= NOPARENT, int alignment= ALIGN_LEFT|ALIGN_TOP):
			fluxChangableLabelBase(_myChangeListener, x,y, 0, parent, alignment),
			isDragging(false)
		{
			enableRelativeMode(true);
			enableVerticalMode(false);
			setRelativeModeSpeed(0.01);
			setMinimumValue(0.01);
			setMaximumValue(10);
			setValue((minimumValue+maximumValue)/2, false);
			setDisplayMode(DM_PLAIN);
		}

		void setRelativeModeSpeed(float speed)
		{ relativeModeSpeed= speed; }

		float getRelativeModeSpeed()
		{ return relativeModeSpeed; }

		void enableRelativeMode(bool enabled)
		{ relativeModeEnabled= enabled; }

		bool isRelativeModeEnabled()
		{ return relativeModeEnabled; }

		void setMinimumValue(float min)
		{ minimumValue= min; }

		void setMaximumValue(float max)
		{ maximumValue= max; }

		void setValue(float val, bool doNotify= true)
		{
			if(val<minimumValue) val= minimumValue;
			else if(val>maximumValue) val= maximumValue;
			value= val;
			if(doNotify) notifyChange();
			updateDisplay();
		}

		float getValue()
		{ return value; }

		void enableVerticalMode(bool enabled)
		{ verticalMode= enabled; }

		void setDisplayMode(DisplayMode mode, int precision= 3)
		{ displayMode= mode; displayPrecision= precision; updateDisplay(); }

	protected:
		virtual void doDrag(int dx, int dy)
		{
			if(relativeModeEnabled)
				setValue(value + (verticalMode? -dy: dx) * relativeModeSpeed);
		}

		virtual void updateDisplay()
		{
			char ch[128];
			switch(displayMode)
			{
				case DM_PLAIN: snprintf(ch, 128, "%.*f", displayPrecision, value); break;
				case DM_SECONDS: snprintf(ch, 128, "%.*fs", displayPrecision, value); break;
				case DM_PERCENTAGE: snprintf(ch, 128, "%.*f%%", displayPrecision, value*100); break;
			}
			setText(ch);
		}


	private:
		float relativeModeSpeed;
		bool relativeModeEnabled;
		float minimumValue;
		float maximumValue;
		float value;
		bool isDragging;
		int dragStartX, dragStartY;
		bool verticalMode;
		enum DisplayMode displayMode;
		int displayPrecision;

		void cbMouse(primitive *self, int type, int x, int y, int btn)
		{
			fluxChangableLabelBase::cbMouse(self, type, x, y, btn);
			if(type==MOUSE_DOWN && btn==MOUSE_BTNLEFT)
			{
				isDragging= true;
				wnd_set_mouse_capture(fluxHandle);
				dragStartX= x; dragStartY= y;
			}
			else if(type==MOUSE_UP)
			{
				isDragging= false;
				wnd_set_mouse_capture(NOWND);
			}
			else if(type==MOUSE_OVER && isDragging)
			{
				rect abspos;
				wnd_get_abspos(fluxHandle, &abspos);
				SDL_WarpMouse(abspos.x+dragStartX, abspos.y+dragStartY);
				doDrag(x-dragStartX, y-dragStartY);
			}
		}
};


class fluxChoiceLabel: public fluxChangableLabelBase
{
	public:
		fluxChoiceLabel(changeListener *_myChangeListener, int x, int y, int parent= NOPARENT, int alignment= ALIGN_LEFT|ALIGN_TOP):
			fluxChangableLabelBase(_myChangeListener, x,y, 0, parent, alignment)
		{
			setText("Choice");
		}

		virtual void selectChoice(int choice, bool doNotify= true)
		{
			if(!choiceList.size()) return;
			while(choice<0) choice+= choiceList.size();
			choiceIndex= choice%choiceList.size();
			setText(choiceList[choiceIndex].c_str());
			if(doNotify) notifyChange();
		}

		void addChoice(const char *choiceText)
		{
			choiceList.push_back(string(choiceText));
			if(choiceList.size()==1) selectChoice(0, false);
		}

		int getChoiceIndex()
		{ return choiceIndex; }

		std::string getChoiceText()
		{ return (choiceIndex<choiceList.size()? choiceList[choiceIndex]: "None"); }

	protected:
		vector<string> choiceList;
		uint32_t choiceIndex;

	private:
		void cbMouse(primitive *self, int type, int x, int y, int btn)
		{
			fluxChangableLabelBase::cbMouse(self, type, x, y, btn);

			if(type==MOUSE_DOWN)
			{
				if(btn==MOUSE_BTN1 || btn==MOUSE_BTNWHEELUP)
					selectChoice(++choiceIndex);
				else if(btn==MOUSE_BTN2 || btn==MOUSE_BTNWHEELDOWN)
					selectChoice(--choiceIndex);
			}
		}
};


class fluxOscWindowConfigPane: public fluxWindowBase, public changeListener
{
	private:
		fluxOscWindow &oscWindow;
		fluxChoiceLabel *triggerTypeChoiceLabel;
		uint32_t triggerTypeText;
		fluxDraggableLabel *displayTimeLabel;
		uint32_t displayTimeText;
		fluxDraggableLabel *verticalScalingLabel;
		uint32_t verticalScalingText;
		fluxDraggableLabel *triggerLevelLabel;
		uint32_t triggerLevelText;

	public:
		fluxOscWindowConfigPane(fluxOscWindow &myOscWindow, int x, int y, int w, int h,
								uint32_t parent= NOPARENT, int alignment= ALIGN_BOTTOM|ALIGN_LEFT|ALIGN_RIGHT):
			fluxWindowBase(x,y, w,h, 0, parent, alignment),
			oscWindow(myOscWindow)
		{
			uint32_t textColor= 0xc8c8c8;
			int textWidth= 85;

			triggerTypeText= create_text(fluxHandle, 8,8, 100,20, "Trigger Type: ", textColor, FONT_DEFAULT);
			triggerTypeChoiceLabel= new fluxChoiceLabel(this, 8+textWidth,8, fluxHandle);
			triggerTypeChoiceLabel->addChoice("Off");
			triggerTypeChoiceLabel->addChoice("Rising Edge");
			triggerTypeChoiceLabel->addChoice("Falling Edge");
			triggerTypeChoiceLabel->selectChoice(!oscWindow.isTriggerEnabled()? 0: oscWindow.isTriggerPositive()? 1: 2);

			triggerLevelText= create_text(fluxHandle, 8,24, 100,20, "Trigger Level: ", textColor, FONT_DEFAULT);
			triggerLevelLabel= new fluxDraggableLabel(this, 8+textWidth,24, fluxHandle);
			triggerLevelLabel->setMinimumValue(-50);
			triggerLevelLabel->setMaximumValue(+50);
			triggerLevelLabel->setRelativeModeSpeed(0.001);
			triggerLevelLabel->enableVerticalMode(true);
			triggerLevelLabel->setValue(oscWindow.getTriggerLevel());

			textWidth= 80;
			displayTimeText= create_text(fluxHandle, 190,8, 100,20, "Display Time: ", textColor, FONT_DEFAULT);
			displayTimeLabel= new fluxDraggableLabel(this, 190+textWidth,8, fluxHandle);
			displayTimeLabel->setMinimumValue(0.0005);
			displayTimeLabel->setMaximumValue(10);
			displayTimeLabel->setRelativeModeSpeed(0.001);
			displayTimeLabel->setDisplayMode(fluxDraggableLabel::DM_SECONDS);
			displayTimeLabel->setValue(oscWindow.getDisplayTime());

			verticalScalingText= create_text(fluxHandle, 190,24, 100,20, "Vert. Scaling: ", textColor, FONT_DEFAULT);
			verticalScalingLabel= new fluxDraggableLabel(this, 190+textWidth,24, fluxHandle);
			verticalScalingLabel->setMinimumValue(0.1);
			verticalScalingLabel->setMaximumValue(100.0);
			verticalScalingLabel->setRelativeModeSpeed(0.005);
			verticalScalingLabel->enableVerticalMode(true);
			verticalScalingLabel->setDisplayMode(fluxDraggableLabel::DM_PERCENTAGE, 0);
			verticalScalingLabel->setValue(oscWindow.getVerticalScaling());
		}

		void updateTriggerLevelDisplay(float newTriggerLevel)
		{ triggerLevelLabel->setValue(newTriggerLevel, false); }
		void updateVerticalScalingDisplay(float newVertScale)
		{ verticalScalingLabel->setValue(newVertScale, false); }
		void updateDisplayTimeDisplay(float newDisplayTime)
		{ displayTimeLabel->setValue(newDisplayTime, false); }

		void valueChanged(changeNotifier *which)
		{
			if(which==triggerTypeChoiceLabel)
			{
				switch(triggerTypeChoiceLabel->getChoiceIndex())
				{
					case 0:
						oscWindow.enableTrigger(false);
						break;
					case 1:
						oscWindow.setTriggerDir(true);
						oscWindow.enableTrigger(true);
						break;
					case 2:
						oscWindow.setTriggerDir(false);
						oscWindow.enableTrigger(true);
						break;
				}
			}
			else if(which==triggerLevelLabel)
				oscWindow.setTriggerLevel(triggerLevelLabel->getValue());
			else if(which==displayTimeLabel)
				oscWindow.setDisplayTime(displayTimeLabel->getValue());
			else if(which==verticalScalingLabel)
				oscWindow.setVerticalScaling(verticalScalingLabel->getValue());
		}
};

// update the GUI to reflect a parameter change of the oscillator window
void fluxOscWindow::updateGuiParam(void *paramAddress)
{
	if(!configPane) return;

	if(paramAddress==&triggerLevel)
		configPane->updateTriggerLevelDisplay(triggerLevel);
	else if(paramAddress==&verticalScaling)
		configPane->updateVerticalScalingDisplay(verticalScaling);
	else if(paramAddress==&displayTime)
		configPane->updateDisplayTimeDisplay(displayTime);
}


const char *getHomeDir()
{
	return getenv("HOME");
}

string getConfigDir()
{
	return string(getHomeDir()) + "/.fluxscope";
}

string getConfigFilename()
{
	return getConfigDir() + "/prefs";
}

bool createConfigDir()
{
	string cfgdir= getConfigDir();
	struct stat st;
	mkdir(cfgdir.c_str(), 0700);
	int err= stat(cfgdir.c_str(), &st);
	if(err==0 && st.st_mode&S_IFDIR)
		return true;
	else
		return false;
}

bool createConfigFile()
{
	if(!createConfigDir()) return false;
	FILE *f= fopen(getConfigFilename().c_str(), "w");
	if(!f) return false;
	fclose(f);
	return true;
}

int main(int argc, char* argv[])
{
	bool doQuit= false;
	double time, lastTime= getTime(), lastJackTry;
	JackInterface JackIF;
	setVideoMode(640, 400);

	fluxOscWindow oscWindow(0,0, 0,64, NOPARENT, ALIGN_LEFT|ALIGN_RIGHT|ALIGN_TOP|ALIGN_BOTTOM);
	if(!gConfigHandler.readFromFile(getConfigFilename().c_str()))
		printf("couldn't read config file %s\n", getConfigFilename().c_str());
	fluxOscWindowConfigPane configPane(oscWindow, 0,0, 0,64, NOPARENT, ALIGN_BOTTOM|ALIGN_LEFT|ALIGN_RIGHT);
	oscWindow.setConfigPane(&configPane);
	JackIF.initialize(1);
	lastJackTry= getTime();
	oscWindow.setSamplingRate(JackIF.getSamplingRate());

	while(!doQuit)
	{
		time= getTime();
		gTime+= time-lastTime;
		lastTime= time;

		SDL_Event ev;
		while(SDL_PollEvent(&ev))
		{
			switch(ev.type)
			{
				case SDL_KEYDOWN:
					if(ev.key.keysym.sym==SDLK_ESCAPE)
						doQuit= true;
					else
						flux_keyboard_event(true, ev.key.keysym.scancode, ev.key.keysym.sym);
					break;
				case SDL_QUIT:
					doQuit= true;
					break;
				case SDL_MOUSEBUTTONDOWN:
					flux_mouse_button_event(SDLMouseButtonToFluxMouseButton(ev.button.button), ev.button.state);
					break;
				case SDL_MOUSEBUTTONUP:
					flux_mouse_button_event(SDLMouseButtonToFluxMouseButton(ev.button.button), ev.button.state);
					break;
				case SDL_MOUSEMOTION:
					flux_mouse_move_event(ev.motion.xrel, ev.motion.yrel);
					break;
				case SDL_VIDEORESIZE:
					setVideoMode(ev.resize.w, ev.resize.h);
					break;

				case SDL_USEREVENT:
					switch(ev.user.code)
					{
						case SDL_USER_ADDJACKBUFFER:
						{
							const JackBufferData *eventData= (JackBufferData*)ev.user.data1;
							for(int i= 0; i<eventData->nChannels; i++)
							{
								oscWindow.addBuffer(eventData->data[i], eventData->nFrames, i);
//								free(eventData->data[i]);
							}
//							free(eventData->data);
//							free(eventData);
							break;
						}
					}
			}
		}

		if(!JackIF.isRunning() && time-lastJackTry>5.0)
		{
			lastJackTry= time;
			JackIF.initialize(1);
		}

		flux_tick();
		glFinish();

		double frametime= getTime() - time;
		double delay= (1.0/100) - frametime;
		if(delay<0.001) delay= 0.001;
		usleep(useconds_t(delay*1000000));
	}

	createConfigFile();
	if(!gConfigHandler.writeToFile(getConfigFilename().c_str()))
		printf("couldn't write to config file %s\n", getConfigFilename().c_str());

	flux_shutdown();
	SDL_Quit();
    return 0;
}
