/*
 * dates.c - calendar maths for Perry.
 *
 * The day-number conversions use Howard Hinnant's well-known
 * "days from civil" algorithms, which work for any Gregorian date.
 */
#include <stdio.h>
#include "dates.h"

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int days_in_month(int year, int month)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && is_leap(year)) return 29;
    return days[month - 1];
}

int valid_date(int year, int month, int day)
{
    return year >= 1 && year <= 9999 && month >= 1 && month <= 12 &&
           day >= 1 && day <= days_in_month(year, month);
}

long long days_from_date(int year, int month, int day)
{
    long long y = year - (month <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void date_from_days(long long z, int *year, int *month, int *day)
{
    long long era, doe, yoe, y, doy, mp, d, m;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = doy - (153 * mp + 2) / 5 + 1;
    m   = mp < 10 ? mp + 3 : mp - 9;
    *year  = (int)(y + (m <= 2));
    *month = (int)m;
    *day   = (int)d;
}

int weekday_of(long long days)
{
    return days >= -4 ? (int)((days + 4) % 7) : (int)((days + 5) % 7 + 6);
}

int parse_date(const char *s, int len, long long *days)
{
    int i, y, m, d;
    if (len != 10 || s[4] != '-' || s[7] != '-') return 0;
    for (i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return 0;
    y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    m = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    if (!valid_date(y, m, d)) return 0;
    *days = days_from_date(y, m, d);
    return 1;
}

void format_date(char *buf, long long days)
{
    int y, m, d;
    date_from_days(days, &y, &m, &d);
    sprintf(buf, "%04d-%02d-%02d", y, m, d);
}

long long add_months(long long days, long long months)
{
    int y, m, d, dim;
    long long total;
    date_from_days(days, &y, &m, &d);
    total = (long long)y * 12 + (m - 1) + months;
    y = (int)(total / 12);
    m = (int)(total % 12) + 1;
    dim = days_in_month(y, m);
    if (d > dim) d = dim;
    return days_from_date(y, m, d);
}

/* Easter Sunday (the "anonymous Gregorian" method). */
static long long easter_sunday(int y)
{
    int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4;
    int f = (b + 8) / 25, g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4, k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    int month = (h + l - 7 * m + 114) / 31;
    int day = ((h + l - 7 * m + 114) % 31) + 1;
    return days_from_date(y, month, day);
}

/* The public holidays that fall on the same date every year. */
static int is_fixed_za_holiday(int m, int d)
{
    static const int fixed[][2] = {
        { 1, 1 },    /* New Year's Day          */
        { 3, 21 },   /* Human Rights Day        */
        { 4, 27 },   /* Freedom Day             */
        { 5, 1 },    /* Workers' Day            */
        { 6, 16 },   /* Youth Day               */
        { 8, 9 },    /* National Women's Day    */
        { 9, 24 },   /* Heritage Day            */
        { 12, 16 },  /* Day of Reconciliation   */
        { 12, 25 },  /* Christmas Day           */
        { 12, 26 },  /* Day of Goodwill         */
    };
    unsigned i;
    for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (fixed[i][0] == m && fixed[i][1] == d) return 1;
    return 0;
}

static int is_za_holiday_itself(long long days)
{
    int y, m, d;
    long long easter;
    date_from_days(days, &y, &m, &d);
    if (is_fixed_za_holiday(m, d)) return 1;
    easter = easter_sunday(y);
    return days == easter - 2      /* Good Friday             */
        || days == easter + 1;     /* Family Day (Monday)     */
}

int is_za_public_holiday(long long days)
{
    if (is_za_holiday_itself(days)) return 1;
    /* A holiday on a Sunday moves the day off to the Monday. */
    return weekday_of(days) == 1 && is_za_holiday_itself(days - 1);
}

int is_business_day(long long days, int use_za_holidays)
{
    int wd = weekday_of(days);
    if (wd == 0 || wd == 6) return 0;
    if (use_za_holidays && is_za_public_holiday(days)) return 0;
    return 1;
}
