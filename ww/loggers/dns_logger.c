#include "dns_logger.h"
#include "hv/hbase.h"
#include "hv/hmutex.h"

struct logger_s
{
    logger_handler     handler;
    unsigned long long bufsize;
    char              *buf;

    int  level;
    int  enable_color;
    char format[64];

    // for file logger
    char               filepath[256];
    unsigned long long max_filesize;
    long               remain_days;
    int                enable_fsync;
    FILE              *fp_;
    char               cur_logfile[256];
    time_t             last_logfile_ts;
    long long          can_write_cnt;

    hmutex_t mutex_; // thread-safe
};

static logger_t     *logger   = NULL;
#define S_GMTOFF 28800 // 8*3600


static void generateLogFileName(const char *filepath, time_t ts, char *buf, int len)
{
    struct tm *tm = localtime(&ts);
    snprintf(buf, len, "%s.%04d%02d%02d.log", filepath, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

static FILE *logFileShift(logger_t *logger)
{
    time_t ts_now        = time(NULL);
    long   interval_days = logger->last_logfile_ts == 0 ? 0
                                                        : (ts_now + S_GMTOFF) / SECONDS_PER_DAY -
                                                            (logger->last_logfile_ts + S_GMTOFF) / SECONDS_PER_DAY;
    if (logger->fp_ == NULL || interval_days > 0)
    {
        // close old logfile
        if (logger->fp_)
        {
            fclose(logger->fp_);
            logger->fp_ = NULL;
        }
        else
        {
            interval_days = 30;
        }

        if (logger->remain_days >= 0)
        {
            char rm_logfile[256] = {0};
            if (interval_days >= logger->remain_days)
            {
                // remove [today-interval_days, today-remain_days] logfile
                for (long i = interval_days; i >= logger->remain_days; --i)
                {
                    time_t ts_rm = ts_now - i * SECONDS_PER_DAY;
                    generateLogFileName(logger->filepath, ts_rm, rm_logfile, sizeof(rm_logfile));
                    remove(rm_logfile);
                }
            }
            else
            {
                // remove today-remain_days logfile
                time_t ts_rm = ts_now - logger->remain_days * SECONDS_PER_DAY;
                generateLogFileName(logger->filepath, ts_rm, rm_logfile, sizeof(rm_logfile));
                remove(rm_logfile);
            }
        }
    }

    // open today logfile
    if (logger->fp_ == NULL)
    {
        generateLogFileName(logger->filepath, ts_now, logger->cur_logfile, sizeof(logger->cur_logfile));
        logger->fp_             = fopen(logger->cur_logfile, "a");
        logger->last_logfile_ts = ts_now;
    }

    // NOTE: estimate can_write_cnt to avoid frequent fseek/ftell
    if (logger->fp_ && --logger->can_write_cnt < 0)
    {
        fseek(logger->fp_, 0, SEEK_END);
        unsigned long long filesize = ftell(logger->fp_);
        if (filesize > logger->max_filesize)
        {
            fclose(logger->fp_);
            logger->fp_ = NULL;
            // ftruncate
            logger->fp_ = fopen(logger->cur_logfile, "w");
            // reopen with O_APPEND for multi-processes
            if (logger->fp_)
            {
                fclose(logger->fp_);
                logger->fp_ = fopen(logger->cur_logfile, "a");
            }
        }
        else
        {
            logger->can_write_cnt =
                ((long long) logger->max_filesize - (long long) filesize) / (long long) logger->bufsize;
        }
    }

    return logger->fp_;
}

static void destroyDnsLogger(void)
{
    if (logger)
    {
        logger_fsync(logger);
        logger_destroy(logger);
        logger = NULL;
    }
}
static void logFileWrite(logger_t *logger, const char *buf, int len)
{
    FILE *fp = logFileShift(logger);
    if (fp)
    {
        fwrite(buf, 1, len, fp);
        if (logger->enable_fsync)
        {
            fflush(fp);
        }
    }
}


static void dnsLoggerHandleWithStdStream(int loglevel, const char *buf, int len)
{
    if (loglevel == LOG_LEVEL_ERROR || loglevel == LOG_LEVEL_FATAL)
    {
        stderr_logger(loglevel, buf, len);
    }
    else
    {
        stdout_logger(loglevel, buf, len);
    }
    logFileWrite(logger, buf, len);
}

static void dnsLoggerHandle(int loglevel, const char *buf, int len)
{
    (void) loglevel;
    logFileWrite(logger, buf, len);
}

logger_t *getDnsLogger()
{
    return logger;
}
void setDnsLogger(logger_t *newlogger)
{
    assert(logger == NULL);
    logger = newlogger;
}

logger_t *createDnsLogger(const char *log_file, bool console)
{
    assert(logger == NULL);
    logger = logger_create();
    logger_set_file(logger, log_file);
    if (console)
    {
        logger_set_handler(logger, dnsLoggerHandleWithStdStream);
    }
    else
    {
        logger_set_handler(logger, dnsLoggerHandle);
    }

    atexit(destroyDnsLogger);
    return logger;
}
