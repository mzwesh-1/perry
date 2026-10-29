/*
 * dates.h - calendar maths.
 *
 * A date is stored as a day number: 0 is 1970-01-01, 1 is 1970-01-02,
 * and so on. That makes "30 days later" a simple addition.
 */
#ifndef PERRY_DATES_H
#define PERRY_DATES_H

long long days_from_date(int year, int month, int day);
void      date_from_days(long long days, int *year, int *month, int *day);

int  valid_date(int year, int month, int day);
int  days_in_month(int year, int month);
int  weekday_of(long long days);             /* 0 = Sunday ... 6 = Saturday */

/* Reads "2026-12-31". Returns 1 if it's a real date. */
int  parse_date(const char *s, int len, long long *days);
void format_date(char *buf, long long days);  /* buf needs 11 bytes */

/* South African public holidays, including the Sunday -> Monday rule. */
int  is_za_public_holiday(long long days);

/* Business days skip Saturdays, Sundays and (if switched on) public holidays. */
int  is_business_day(long long days, int use_za_holidays);

/* Adds months, keeping the day if it exists: 31 Jan + 1 month = 28/29 Feb. */
long long add_months(long long days, long long months);

#endif
