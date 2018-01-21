/*
 * suspendoutput.c: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id: suspendoutput.c,v 1.23 2016/12/31 20:26:54 phintuka Exp $
 */

#include <getopt.h>

#include <vdr/plugin.h>
#include <vdr/thread.h>
#include <vdr/status.h>
#include <vdr/menuitems.h>
#include <vdr/i18n.h>

#include <vdr/recording.h>  // RemoveDeletedRecordings()
#include <vdr/menu.h>       // cRecordControls::Active()
#include <vdr/cutter.h>     // cCutter::Active()
#include <vdr/interface.h>  // Interface->HasSVDRPConnection()

#include "dummy_player.h"


//#define TRACE(x...) isyslog(x)
#define TRACE(x...)


static const char *VERSION         = "2.1.0";
static const char *DESCRIPTION     = trNOOP("Suspend output");
static const char *MAINMENUENTRY1  = trNOOP("Suspend Output");
static const char *MAINMENUENTRY2  = trNOOP("Resume Output");
static const int   INACTIVITY_TIMER_DEFAULT = 2*60*60; // 2 hours


// --- cMenuSetupSuspendoutput ----------------------------------------------

class cMenuSetupSuspendoutput : public cMenuSetupPage
{
  private:
    int timer, *ptimer;
    int showmenu;
    bool *pshowmenu;
    int   paused;
    bool *ppaused;
    int logo;
  protected:
    virtual void Store(void)
    {
      SetupStore("SuspendTimer", *ptimer = 60*timer);
      SetupStore("MainMenu", (*pshowmenu = showmenu) ? true : false);
      SetupStore("SuspendPaused", (*ppaused = paused) ? true : false);
      SetupStore("ShowLogo", logo);
      cDummyPlayerControl::UseBlankImage = (logo ? false : true);
    }

  public:
    cMenuSetupSuspendoutput(int *piTimer, bool *pbShowmenu, bool *pbPaused) :
      timer(*piTimer / 60), ptimer(piTimer),
      showmenu(*pbShowmenu ? 1 : 0), pshowmenu(pbShowmenu),
      paused(*pbPaused ? 1 : 0), ppaused(pbPaused)
    {
      Add(new cMenuEditBoolItem(tr("Show in main menu"), &showmenu));
      Add(new cMenuEditIntItem(tr("Inactivity timer (min)"), &timer,
                               0, 7*24*60));
      Add(new cMenuEditBoolItem(tr("Suspend when replay is paused"), &paused));

      logo = ! cDummyPlayerControl::UseBlankImage;
      Add(new cMenuEditBoolItem(tr("Show VDR logo when suspended"),
                                &logo));
    }
};

// --- cPluginSetupSuspendoutput --------------------------------------------

class cPluginSuspendoutput : public cPlugin, cStatus
{
  //
  // Status monitor interface
  //

  protected:

    virtual void Replaying(const cControl *Control, const char *Name,
                           const char *File, bool On)
    {
      if(On && Name && (strlen(Name)!=4 || strncmp("none",Name,4))) {
        TRACE("suspendoutput: replaying -> true");
        m_bReplaying = true;
        Activity();
      } else {
        TRACE("suspendoutput: replaying -> false");
        m_bReplaying = false;
      }
    }

    virtual void SetVolume(int Volume, bool Absolute)        { Activity(); }
    virtual void OsdClear(void)                              { Activity(); }
    virtual void OsdTitle(const char *Title)                 { Activity(); }
    virtual void OsdStatusMessage(const char *Message)       { Activity(); }
    virtual void OsdItem(const char *Text, int Index)        { Activity(); }
    virtual void OsdCurrentItem(const char *Text)            { Activity(); }
    virtual void OsdTextItem(const char *Text, bool Scroll)  { Activity(); }
    virtual void OsdChannel(const char *Text)                { Activity(); }
    virtual void OsdProgramme(time_t PresentTime, const char *PresentTitle,
                              const char *PresentSubtitle,
                              time_t FollowingTime,
                              const char *FollowingTitle,
                              const char *FollowingSubtitle) { Activity(); }

  //
  // Plugin interface
  //

  private:
    bool m_bMenu;
    bool m_bReplaying;
    int  m_iInactivityTimerTimeout;
    int  m_iSecondsSinceLastAction;
    bool m_bPlayerLaunchedByTimer;
    bool m_bSuspendWhenPaused;
    cMutex m_Lock;
    time_t m_Timer;
    enum { REQ_NONE = 0, REQ_SUSPEND, REQ_RESUME } m_SvcReq;  // Pending request from service interface

  public:
    cPluginSuspendoutput(void);
    virtual ~cPluginSuspendoutput();
    virtual const char *Version(void) { return VERSION; }
    virtual const char *Description(void) { return DESCRIPTION; }
    virtual const char *CommandLineHelp(void);
    virtual bool ProcessArgs(int argc, char *argv[]);
    virtual bool Initialize(void) {return true;};
    virtual bool Start(void);
    virtual void Stop(void);
    virtual void MainThreadHook(void);
    virtual void Housekeeping(void) {};
    virtual const char *MainMenuEntry(void)
    {
      return (!m_bMenu) ? NULL :
                          cDummyPlayerControl::IsOpen() ? MAINMENUENTRY2 :
                                                          MAINMENUENTRY1;
    }

 protected:
    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *Name, const char *Value);

    virtual bool Service(const char *Id, void *Data);

    virtual const char **SVDRPHelpPages(void);
    virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);

  //
  // internal
  //

  protected:

    void Activity(void);
    void CheckInactivityTimer(void);
};

cPluginSuspendoutput::cPluginSuspendoutput(void)
{
  m_bMenu = true;
  m_bReplaying = false;
  m_iSecondsSinceLastAction = 0;
  m_iInactivityTimerTimeout = INACTIVITY_TIMER_DEFAULT;
  m_bPlayerLaunchedByTimer = false;
  m_bSuspendWhenPaused = false;
  m_Timer = 0;
  m_SvcReq = REQ_NONE;
}

cPluginSuspendoutput::~cPluginSuspendoutput()
{
}

const char *cPluginSuspendoutput::CommandLineHelp(void)
{
  static const char CmdLineHelp[] =
    "  -m        --menu         Show suspend/resume entry in main menu\n"
    "                           (default)\n"
    "  -M        --nomenu       Don't show entry in main menu\n"
    "  -t MIN    --timer=MIN    Suspend output after MIN minutes of\n"
    "                           inactivity\n"
    "                           (default: 120 minutes)\n"
    "  -T        --notimer      Disable inactivity timer\n"
    "  -p        --paused       Allow suspend when replay is paused\n"
    "  -l        --logo         Show VDR logo when suspended\n"
    "  -b        --blank        Blank screen when suspended\n"
    ;

  return CmdLineHelp;
}

bool cPluginSuspendoutput::ProcessArgs(int argc, char *argv[])
{
  static struct option long_options[] = {
    { "menu",     no_argument,       NULL, 'm' },
    { "nomenu",   no_argument,       NULL, 'M' },
    { "timer",    required_argument, NULL, 't' },
    { "notimer",  no_argument,       NULL, 'T' },
    { "paused",   no_argument,       NULL, 'p' },
    { "logo",     no_argument,       NULL, 'l' },
    { "blank",    no_argument,       NULL, 'b' },
    { NULL }
  };

  int c;
  while ((c = getopt_long(argc, argv, "mMt:T", long_options, NULL)) != -1) {
    switch (c) {
    case 'm': m_bMenu = true;
              isyslog("suspendoutput: main menu entry enabled");
              SetupStore("MainMenu", 1);
              break;
    case 'M': m_bMenu = false;
              isyslog("suspendoutput: main menu entry disabled");
              SetupStore("MainMenu", 0);
              break;
    case 't': m_iInactivityTimerTimeout = atoi(optarg) * 60;
              isyslog("suspendoutput: inactivity timer set to %d minutes",
                      m_iInactivityTimerTimeout/60);
              SetupStore("SuspendTimer", m_iInactivityTimerTimeout);
              break;
    case 'T': m_iInactivityTimerTimeout = 0;
              isyslog("suspendoutput: inactivity timer disabled");
              SetupStore("SuspendTimer", 0);
              break;
    case 'p': m_bSuspendWhenPaused = true;
              SetupStore("SuspendPaused", 1);
              break;
    case 'l': cDummyPlayerControl::UseBlankImage = false;
              SetupStore("ShowLogo", 1);
              break;
    case 'b': cDummyPlayerControl::UseBlankImage = true;
              SetupStore("ShowLogo", 0);
              break;
    default:  return false;
    }
  }
  return true;
}


bool cPluginSuspendoutput::SetupParse(const char *Name, const char *Value)
{
  if (!strcasecmp(Name, "SuspendTimer")) {
    m_iInactivityTimerTimeout = atoi(Value);
    isyslog("suspendoutput: inactivity timer set to %d minutes",
            m_iInactivityTimerTimeout/60);
  } else if (!strcasecmp(Name, "MainMenu")) {
    m_bMenu = atoi(Value) ? true : false;
  } else if (!strcasecmp(Name, "SuspendPaused")) {
    m_bSuspendWhenPaused = atoi(Value) ? true : false;
  } else if (!strcasecmp(Name, "ShowLogo")) {
    cDummyPlayerControl::UseBlankImage = atoi(Value) ? false : true;
  } else
    return false;
  return true;
}

bool cPluginSuspendoutput::Start(void)
{
  return true;
}

void cPluginSuspendoutput::Stop(void)
{
  if(cDummyPlayerControl::IsOpen())
    cDummyPlayerControl::Close();
}

void cPluginSuspendoutput::MainThreadHook(void)
{
  time_t now = time(NULL);
  if (now - m_Timer > 60) {
    if (m_Timer > 0)
      CheckInactivityTimer();
    m_Timer = now;
  }

  /* handle service requests */
  m_Lock.Lock();

  switch (m_SvcReq) {
    case REQ_RESUME:
      isyslog("suspendoutput: resuming (service request)");
      cDummyPlayerControl::Close();
      break;

    case REQ_SUSPEND:
      if (cDummyPlayerControl::IsOpen()) {
        isyslog("suspendoutput: service request SUSPEND ignored (already suspended)");
      } else if (m_bReplaying) {
        isyslog("suspendoutput: service request SUSPEND ignored (replaying)");
      } else {
        isyslog("suspendoutput: suspending (service request)");
        cControl::Launch(new cDummyPlayerControl);
      }
      break;

    case REQ_NONE:
    default:
      break;
  }
  m_SvcReq = REQ_NONE;

  m_Lock.Unlock();
}

void cPluginSuspendoutput::CheckInactivityTimer(void)
{
  TRACE("suspendoutput: CheckInactivityTimer");

  if(m_bReplaying &&
     m_bSuspendWhenPaused &&
     (!cDummyPlayerControl::IsOpen()) ) {
    bool Play, Forward;
    int Speed;
    if(cControl::Control() &&
       cControl::Control()->GetReplayMode(Play, Forward, Speed) &&
       Play)
      Activity();
  }

  m_Lock.Lock();
  m_iSecondsSinceLastAction += 60;
  if(m_iInactivityTimerTimeout &&
     m_iSecondsSinceLastAction >= m_iInactivityTimerTimeout &&
     (!m_bReplaying || m_bSuspendWhenPaused) &&
     (!cDummyPlayerControl::IsOpen())) {
    isyslog("suspendoutput: output suspended by inactivity timer");
    cControl::Launch(new cDummyPlayerControl);
    m_bPlayerLaunchedByTimer = true;
  }
  m_Lock.Unlock();

  //
  // Perform vdr housekeeping tasks
  // (vdr does not do this when player is active)
  //
  static time_t LastActivity = 0;
  if(!cDummyPlayerControl::IsOpen()) {
    LastActivity = time(NULL);
  }
  if(cDummyPlayerControl::IsOpen()) {

    if ( !cRecordControls::Active()
#if VDRVERSNUM < 20200
         && !cCutter::Active()
#endif
#if VDRVERSNUM < 20301
         && !Interface->HasSVDRPConnection()
#endif
         ) {

#define ACTIVITYTIMEOUT 600   // 10 min

      time_t Now = time(NULL);
      if (Now - LastActivity > ACTIVITYTIMEOUT) {
        TRACE("suspendoutput: Performing housekeeping tasks...");
#if 0
        // TODO:
        // add command line option --shutdown to enable this
        // Shutdown:
        if (Shutdown && Setup.MinUserInactivity &&
            Now - LastActivity > Setup.MinUserInactivity * 60) {
          cTimer *timer = Timers.GetNextActiveTimer();
          time_t Next  = timer ? timer->StartTime() : 0;
          time_t Delta = timer ? Next - Now : 0;

          if (!Next || Delta > Setup.MinEventTimeout * 60) {
            if (timer)
              dsyslog("suspendoutput: next timer event at %s",
                      *TimeToString(Next));
              dsyslog("suspendoutput: trying to shutdown ...");
cRemote::Put(kPower);
          }
        }
#endif

        // Disk housekeeping:
        RemoveDeletedRecordings();
        cSchedules::Cleanup();
#if VDRVERSNUM >= 20301
        // Disk housekeeping:
        ListGarbageCollector.Purge();
#endif
        // Plugins housekeeping:


        //PluginManager.Housekeeping();
        static int nextHousekeeping = 0;
        //static time_t lastHousekeeping = Now;
        //#define HOUSEKEEPINGDELTA 10 // seconds

        //if (Now - lastHousekeeping > HOUSEKEEPINGDELTA) {
          cPlugin *plugin = cPluginManager::GetPlugin(nextHousekeeping++);
          if (!plugin)
            cPluginManager::GetPlugin(nextHousekeeping = 0);
          if (plugin)
            plugin->Housekeeping();
          //lastHousekeeping = time(NULL);
        //}

        TRACE("suspendoutput: Performing housekeeping tasks done.");
      }
    }
  }
}

void cPluginSuspendoutput::Activity(void)
{
  TRACE("suspendoutput: Activity detected");

  m_Lock.Lock();
  m_iSecondsSinceLastAction = 0;
  if(m_bPlayerLaunchedByTimer)
    if(cDummyPlayerControl::IsOpen()) {
      isyslog("suspendoutput: output resumed by user action");
      cDummyPlayerControl::Close();
    }
  m_Lock.Unlock();
}

cOsdObject *cPluginSuspendoutput::MainMenuAction(void)
{
  // Perform the action when selected from the main VDR menu.
  m_Lock.Lock();
  Activity();
  m_bPlayerLaunchedByTimer = false;
  if(! cDummyPlayerControl::IsOpen()) {
    isyslog("suspendoutput: output suspended by user");
    cControl::Launch(new cDummyPlayerControl);
  } else {
    isyslog("suspendoutput: output resumed by user");
    cDummyPlayerControl::Close();
  }
  m_Lock.Unlock();
  return NULL;
}

cMenuSetupPage *cPluginSuspendoutput::SetupMenu(void)
{
  return new cMenuSetupSuspendoutput(&m_iInactivityTimerTimeout,
                                     &m_bMenu,
                                     &m_bSuspendWhenPaused);
}

/*
 * SVDRP interface
 */

const char **cPluginSuspendoutput::SVDRPHelpPages(void)
{
  static const char *HelpPages[] = {
    "SUSPEND\n"
    "    Suspend output (video playback).",
    "RESUME\n"
    "    Resume output.\n",
    "STATUS\n"
    "    Query current suspend status.\n",
    NULL
  };
  return HelpPages;
}

cString cPluginSuspendoutput::SVDRPCommand(const char *Command, const char *Option, int &ReplyCode)
{
  if (strcasecmp(Command, "SUSPEND") == 0) {
    m_Lock.Lock();
    m_SvcReq = REQ_SUSPEND;
    m_Lock.Unlock();
    return cString("Suspend request queued");

  } else if (strcasecmp(Command, "RESUME") == 0) {
    m_Lock.Lock();
    m_SvcReq = REQ_RESUME;
    m_Lock.Unlock();
    return cString("Resume request queued");

  } else if (strcasecmp(Command, "STATUS") == 0) {
    bool suspended = cDummyPlayerControl::IsOpen();
    return cString::sprintf("Output is %ssuspended", suspended ? "" : "not ");
  }

  return NULL;
}

/*
 * Service interface
 */

bool cPluginSuspendoutput::Service(const char *Id, void *Data)
{
  if (!strcmp(Id, "SuspendOutputPlugin-v1.0")) {
    if (Data) {
      const char *cmd = (const char *)Data;
      if (!strcmp(cmd, "Suspend")) {
        dsyslog("suspendoutput: suspend requested by service");
        m_Lock.Lock();
        m_SvcReq = REQ_SUSPEND;
        m_Lock.Unlock();
        return true;
      }
      if (!strcmp(cmd, "Resume")) {
        dsyslog("suspendoutput: resume requested by service");
        m_Lock.Lock();
        m_SvcReq = REQ_RESUME;
        m_Lock.Unlock();
        return true;
      }
    }
  }
  return false;
}

VDRPLUGINCREATOR(cPluginSuspendoutput); // Don't touch this!
