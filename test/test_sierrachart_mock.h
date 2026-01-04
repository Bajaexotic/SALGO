// ============================================================================
// test_sierrachart_mock.h
// Mock Sierra Chart types for standalone unit testing
// Only used when TEST_MODE is defined (not in actual Sierra Chart compilation)
// ============================================================================

#ifndef SIERRACHART_H
#define SIERRACHART_H
// Also prevent inclusion of real Sierra Chart header
#ifndef _SIERRACHART_H_
#define _SIERRACHART_H_
#endif

#include <cmath>
#include <string>

// ============================================================================
// SCString Mock (minimal implementation) - Must be before SCDateTime
// ============================================================================

#define FORMAT_DATE_TIME_MS 0

struct SCString {
private:
    std::string m_str;

public:
    SCString() {}
    SCString(const char* s) : m_str(s ? s : "") {}
    SCString(const std::string& s) : m_str(s) {}

    const char* GetChars() const { return m_str.c_str(); }
    int GetLength() const { return static_cast<int>(m_str.length()); }

    SCString& operator=(const char* s) {
        m_str = s ? s : "";
        return *this;
    }

    SCString& operator+=(const char* s) {
        if (s) m_str += s;
        return *this;
    }

    SCString& operator+=(char c) {
        m_str += c;
        return *this;
    }

    // Format function (simplified - not full printf support)
    void Format(const char* fmt, ...) {
        // For testing, just store the format string
        m_str = fmt ? fmt : "";
    }
};

// ============================================================================
// SCDateTime Mock
// Sierra Chart stores date-time as days since 1899-12-30 (Excel format)
// The integer part is the date, fractional part is time (1.0 = 24 hours)
// ============================================================================

struct SCDateTime {
    double m_dt = 0.0;

    SCDateTime() : m_dt(0.0) {}
    SCDateTime(double dt) : m_dt(dt) {}

    // Set date/time from components
    // Uses Excel-style date serial: days since 1899-12-30
    void SetDateTime(int year, int month, int day, int hour, int minute, int second) {
        // Calculate days since 1899-12-30 (Excel serial date)
        // This is an accurate calculation matching Sierra Chart's behavior

        // Days from years (approximate but consistent)
        int days = (year - 1900) * 365 + (year - 1900) / 4;

        // Days from months
        static const int cumDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        days += cumDays[month - 1];

        // Add leap day if after Feb in a leap year
        if (month > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
            days += 1;
        }

        // Days from day of month
        days += day;

        // Add 1 because Excel counts from Dec 30, 1899 (not Dec 31)
        days += 1;

        // Time as fraction of day (this is the critical part for test accuracy)
        double timeFraction = (static_cast<double>(hour) * 3600.0 +
                               static_cast<double>(minute) * 60.0 +
                               static_cast<double>(second)) / 86400.0;

        m_dt = static_cast<double>(days) + timeFraction;
    }

    double GetAsDouble() const { return m_dt; }
    void SetAsDouble(double dt) { m_dt = dt; }

    // Arithmetic operators
    SCDateTime operator+(double days) const {
        SCDateTime r;
        r.m_dt = m_dt + days;
        return r;
    }

    SCDateTime operator-(double days) const {
        SCDateTime r;
        r.m_dt = m_dt - days;
        return r;
    }

    SCDateTime& operator+=(double days) {
        m_dt += days;
        return *this;
    }

    SCDateTime& operator-=(double days) {
        m_dt -= days;
        return *this;
    }

    // Static helper functions (matching Sierra Chart API)
    static double SECONDS(int s) { return s / 86400.0; }
    static double MINUTES(int m) { return m / 1440.0; }
    static double HOURS(int h) { return h / 24.0; }
    static double DAYS(int d) { return static_cast<double>(d); }

    // Time component extraction (simplified)
    int GetHour() const {
        double frac = m_dt - std::floor(m_dt);
        return static_cast<int>(frac * 24.0);
    }

    int GetMinute() const {
        double frac = m_dt - std::floor(m_dt);
        double hours = frac * 24.0;
        double minutes = (hours - std::floor(hours)) * 60.0;
        return static_cast<int>(minutes);
    }

    int GetSecond() const {
        double frac = m_dt - std::floor(m_dt);
        double totalSeconds = frac * 86400.0;
        return static_cast<int>(totalSeconds) % 60;
    }

    // Time in seconds since midnight
    int GetTimeInSeconds() const {
        double frac = m_dt - std::floor(m_dt);
        return static_cast<int>(frac * 86400.0);
    }

    // Date component extraction (reverse of SetDateTime)
    int GetYear() const {
        int days = static_cast<int>(m_dt);
        // Approximate reverse calculation (good enough for testing)
        int year = 1900 + (days - 1) / 365;
        // Adjust for leap years
        while (true) {
            int testDays = (year - 1900) * 365 + (year - 1900) / 4 + 1;
            if (testDays > days) {
                year--;
            } else {
                break;
            }
        }
        return year;
    }

    int GetMonth() const {
        int days = static_cast<int>(m_dt);
        int year = GetYear();
        int yearDays = (year - 1900) * 365 + (year - 1900) / 4 + 1;
        int dayOfYear = days - yearDays + 1;

        static const int cumDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
        bool isLeap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));

        for (int m = 1; m <= 12; m++) {
            int cumEnd = cumDays[m];
            if (isLeap && m > 2) cumEnd++;
            if (dayOfYear <= cumEnd) return m;
        }
        return 12;
    }

    int GetDay() const {
        int days = static_cast<int>(m_dt);
        int year = GetYear();
        int month = GetMonth();
        int yearDays = (year - 1900) * 365 + (year - 1900) / 4 + 1;
        int dayOfYear = days - yearDays + 1;

        static const int cumDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        bool isLeap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        int cumPrev = cumDays[month - 1];
        if (isLeap && month > 2) cumPrev++;

        return dayOfYear - cumPrev;
    }

    // Get date as YYYYMMDD integer
    int GetDate() const {
        return GetYear() * 10000 + GetMonth() * 100 + GetDay();
    }

    // Check if date is set (non-zero)
    bool IsDateSet() const { return m_dt > 0.0; }

    // Check if date is unset (zero)
    bool IsUnset() const { return m_dt == 0.0; }

    // Set to "now" (stub for testing - uses a fixed time)
    void SetToNow() { m_dt = 45658.5; }  // Arbitrary test value (~2025)

    // Format as string (stub for testing)
    SCString GetDateTimeAsString(int format) const {
        return SCString("2024-01-01 09:30:00");  // Stub return
    }
};

// ============================================================================
// SCFloatArray Mock (matches Sierra Chart array type)
// ============================================================================

#include <vector>

struct SCFloatArray {
private:
    std::vector<float> m_data;

public:
    SCFloatArray() {}
    SCFloatArray(int size) : m_data(size, 0.0f) {}

    int GetArraySize() const { return static_cast<int>(m_data.size()); }

    void SetArraySize(int size) {
        m_data.resize(size, 0.0f);
    }

    float& operator[](int index) {
        if (index < 0 || index >= static_cast<int>(m_data.size())) {
            static float dummy = 0.0f;
            return dummy;
        }
        return m_data[index];
    }

    float operator[](int index) const {
        if (index < 0 || index >= static_cast<int>(m_data.size())) {
            return 0.0f;
        }
        return m_data[index];
    }
};

// ============================================================================
// SCStudyInterfaceRef Mock (minimal stub)
// ============================================================================

struct s_sc;
using SCStudyInterfaceRef = s_sc&;

struct s_sc {
    // Minimal stub for logging
    void AddMessageToLog(const char* msg, int showLog) {
        // No-op for testing
        (void)msg;
        (void)showLog;
    }

    void AddMessageToLog(const SCString& msg, int showLog) {
        // No-op for testing
        (void)msg;
        (void)showLog;
    }

    // Minimal stub for chart drawing
    int UseTool(void* tool, int lineNumber) {
        (void)tool;
        (void)lineNumber;
        return 0;
    }
};

// ============================================================================
// s_UseTool Mock (minimal stub for chart tools)
// ============================================================================

struct s_UseTool {
    int ChartNumber = 0;
    int DrawingType = 0;
    int LineNumber = 0;
    int BeginIndex = 0;
    float BeginValue = 0.0f;
    int EndIndex = 0;
    float EndValue = 0.0f;
    unsigned int Color = 0;
    int LineWidth = 1;
    int LineStyle = 0;
    int AddMethod = 0;
    SCString Text;

    // Constants
    static const int DRAWING_HORIZONTAL_LINE = 1;
    static const int UTAM_ADD_OR_ADJUST = 1;
};

// ============================================================================
// s_VolumeAtPriceV2 Mock (matches Sierra Chart VAPContainer.h)
// ============================================================================

struct s_VolumeAtPriceV2 {
    int PriceInTicks;
    unsigned int Volume;
    unsigned int BidVolume;
    unsigned int AskVolume;
    unsigned int NumberOfTrades;

    s_VolumeAtPriceV2()
        : PriceInTicks(0)
        , Volume(0)
        , BidVolume(0)
        , AskVolume(0)
        , NumberOfTrades(0)
    {}

    bool IsEmpty() const {
        return PriceInTicks == 0 && Volume == 0;
    }
};

#endif // SIERRACHART_H
