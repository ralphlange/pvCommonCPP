#include "mb.h"

#include <time.h>
#include <map>
#include <math.h>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <iomanip>

#include <epicsMutex.h>

// TODO clean this up
#if defined(__APPLE__)
#include <mach/mach_time.h>
uint64_t MBTime()
{
    return mach_absolute_time();
}
#else
//#include <sys/time.h>
uint64_t MBTime()
{
     struct timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
     return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + static_cast<uint64_t>(ts.tv_nsec);
//     struct timeval tv;
//     gettimeofday(&tv, NULL);
//     return static_cast<uint64_t>(tv.tv_sec) * 1000000000 + static_cast<uint64_t>(tv.tv_usec) * 1000;
}
#endif

#ifdef vxWorks
#define GETPID() taskIdSelf()
#else
#define GETPID() getpid()
#endif

void MBPointAdd(MBEntity &e, intptr_t id, uint8_t stage)
{
    // no copy and no MBPoint init solution
    const std::size_t ix = e.pos.fetch_add(1);
    MBPoint& p = e.points[ix];
    p.id = id; p.stage = stage;
    p.time = MBTime();
}

void MBCSVExport(MBEntity &e, uint8_t stageOnly, std::size_t skipFirstNSamples, std::ostream &o)
{
    std::size_t iterationCount = 0;
    const std::size_t len = e.pos.load(); 
    for (std::size_t i = 0; i < len; i++)
    {
        MBPoint& p = e.points[i];

        // first stage is start time, skip
        if (p.stage == 0)
        {
            // new first stage means new iteration
            iterationCount++;
            // do not skip here
        }

        // process only one stage, if stageOnly is not 0
        if (stageOnly != 0 && stageOnly != p.stage)
            continue;

        // skip fist N iterations
        // NOTE: we assume stages (iterations) are ordered
        if (iterationCount <= skipFirstNSamples)
            continue;

        o << p.id << ',' << static_cast<uint32_t>(p.stage) << ',' << p.time << std::endl;
    }
}

// NOTE: this method is not thread-safe
void MBCSVImport(MBEntity &e, std::istream &i)
{
    std::string line;
    char c;    

    e.points.resize(0);
    size_t lc = 0;
    while (getline(i,line))
    {
        lc++;
        std::istringstream is(line);
        MBPoint p;
        is >> p.id >> c;
        uint32_t s; is >> s >> c; p.stage = s;
        is >> p.time;
        
        if (is.good() || is.eof())        
            e.points.push_back(p);
        else
        {
            std::cerr << "failed to parse line " << lc << ": \"" << line << "\"" << std::endl;
        }
    }
    e.pos = e.points.size();
}

void MBNormalize(MBEntity &e)
{
    std::map<intptr_t, uint64_t> lastTime;
    
    const std::size_t len = e.pos.load(); 
    for (std::size_t i = 0; i < len; i++)
    {
        MBPoint& p = e.points[i];
        if (p.stage == 0)
            lastTime[p.id] = p.time;
            
        std::map<intptr_t, uint64_t>::iterator last = lastTime.find(p.id);
        if (last == lastTime.end())
        {
            std::cerr << "no 0 stage for " << e.name << ", id = " << p.id << std::endl;
            p.time = -1;    // TODO report error?!
        }
        else
        {
            uint64_t lt = last->second;
            last->second = p.time;
            p.time -= lt;
        }
    }
}

struct MBStatistics
{
    std::size_t count;
    uint64_t min;
    uint64_t max;
    double sum;

    double mean;
    double stdev;
    
    MBStatistics() :
        count(0),
        min(std::numeric_limits<uint64_t>::max()),
        max(std::numeric_limits<uint64_t>::min()),
        sum(0.0),
        mean(0.0),
        stdev(0.0)
    {}

    MBStatistics(uint64_t sample) :
        count(1),
        min(sample),
        max(sample),
        sum(sample),
        mean(0.0),
        stdev(0.0)
    {
    }

    void addSample(uint64_t sample)
    {
        count++;
        if (sample < min) min = sample;
        if (sample > max) max = sample;
        sum += sample;
    }

    void pass1Epilogue()
    {
        mean = sum/(double)count;
    }

    void addSamplePass2(uint64_t sample)
    {
        double diff = sample - mean;
        stdev += diff*diff;
    }

    void pass2Epilogue()
    {
        stdev = sqrt(stdev/(double)count);
    }

};

typedef std::map<uint8_t, MBStatistics> StatsMapPerStage;

void MBStats(MBEntity &e, uint8_t stageOnly, std::size_t skipFirstNSamples, std::ostream &o)
{
    MBNormalize(e);
    
    StatsMapPerStage stats;
    std::size_t iterationCount = 0;

    // pass 1
    const std::size_t len = e.pos.load(); 
    for (std::size_t i = 0; i < len; i++)
    {
        MBPoint& p = e.points[i];
        
        // first stage is start time, skip
        if (p.stage == 0)
        {
            // new first stage means new iteration
            iterationCount++;
            continue;
        }
        // process only one stage, if stageOnly is not 0
        else if (stageOnly != 0 && stageOnly != p.stage)
            continue;

        // skip fist N iterations
        // NOTE: we assume stages (iterations) are ordered
        if (iterationCount <= skipFirstNSamples)
            continue;

        StatsMapPerStage::iterator s = stats.find(p.stage);
        if (s == stats.end())
            stats[p.stage] = MBStatistics(p.time);
        else
            s->second.addSample(p.time);
    }
    
    for (StatsMapPerStage::iterator i = stats.begin();
         i != stats.end();
         i++)
        i->second.pass1Epilogue();



    iterationCount = 0;

    // pass 2
    for (std::size_t i = 0; i < len; i++)
    {
        MBPoint& p = e.points[i];

        // first stage is start time, skip
        if (p.stage == 0)
        {
            // new first stage means new iteration
            iterationCount++;
            continue;
        }
        // process only one stage, if stageOnly is not 0
        else if (stageOnly != 0 && stageOnly != p.stage)
            continue;

        // skip fist N iterations
        // NOTE: we assume stages (iterations) are ordered
        if (iterationCount <= skipFirstNSamples)
            continue;

        stats[p.stage].addSamplePass2(p.time);
    }

    for (StatsMapPerStage::iterator i = stats.begin();
         i != stats.end();
         i++)
        i->second.pass2Epilogue();


    uint64_t smin = 0;
    uint64_t smax = 0;
    double smean = 0;
    double sstdev = 0;

    for (StatsMapPerStage::iterator i = stats.begin();
         i != stats.end();
         i++)
    {
        smin += i->second.min;
        smax += i->second.max;
        smean += i->second.mean;
        sstdev += i->second.stdev;

        o << "stage " << std::setw(3) << static_cast<uint32_t>(i->first)
                      << ": min = " << std::setw(9) << i->second.min
                      << ", max = " << std::setw(9) << i->second.max
                      << ", mean = " << std::setw(9) << static_cast<uint64_t>(i->second.mean)
                      << ", stdev = " << std::setw(9) << static_cast<uint64_t>(i->second.stdev) << std::endl;
    }
    
    o << std::string(80,'-') << std::endl;
    
    o << "stage " << std::setw(3) << "sum"
                    << ": min = " << std::setw(9) << smin
                    << ", max = " << std::setw(9) << smax
                    << ", mean = " << std::setw(9) << static_cast<uint64_t>(smean)
                    << ", stdev = " << std::setw(9) << static_cast<uint64_t>(sstdev) << std::endl;
}

typedef std::vector<MBEntity*> EntitiesVector;

static int nifty_counter;
static epicsMutex* MBMutex;

// The counter is initialized at load-time, i.e., before any of the static objects are initialized.
MBMutexInitializer::MBMutexInitializer ()
{
    if (0 == nifty_counter++)
    {
        // Initialize static members.
        MBMutex = new epicsMutex();
    }
}

MBMutexInitializer::~MBMutexInitializer ()
{
    if (0 == --nifty_counter)
    {
        // Clean-up.
        delete MBMutex;
    }
}

class MutexLock {
public:

    explicit MutexLock(epicsMutex &m) :
        mutexPtr(m),
        locked(true)
    {
        mutexPtr.lock();
    }

    ~MutexLock()
    {
        unlock();
    }

    void lock()
    {
        if (!locked)
        {
            mutexPtr.lock();
            locked = true;
        }
    }
    void unlock()
    {
        if (locked)
        {
            mutexPtr.unlock();
            locked = false;
        }
    }

private:
    epicsMutex &mutexPtr;
    bool locked;
};


void MBEntityRegister(MBEntity *e)
{
    MutexLock lock(*MBMutex);
    static EntitiesVector MBEntities;

    if (e)
    {
        MBEntities.push_back(e);
    }
    else
    {
        for(EntitiesVector::const_iterator i = MBEntities.begin();
            i != MBEntities.end();
            i++)
        {
            // skip empty entities
            if ((*i)->pos)
            {
                char fileName[1024];
                char* path = getenv("MB_OUTPUT_DIR");
                if (path == 0) path = const_cast<char*>(".");
                // NOTE: snprintf not used because of older VxWorks
                sprintf(fileName, "%s/mb_%s_%d.csv", path, (*i)->name.c_str(), GETPID());
                std::ofstream out(fileName);
                if (out.is_open())
                {
                    MBCSVExport(*(*i), 0, 0, out);
                    out.close();
                }
                else
                {
                    std::cerr << "failed to create a file " << fileName << ", skipping..." << std::endl;
                }
            }
        }
    }
}

void MBAtExit()
{
    MBEntityRegister(0);
}


void MBInit()
{
    MutexLock lock(*MBMutex);
    static bool inited = false;
    if (!inited)
    {
        inited = true;
        atexit(MBAtExit);
    }
}

