/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * MuseScore Agent
 * Copyright (C) 2026 Liam Green and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "ast.h"

#include <numeric>
#include <stdexcept>

using namespace mu::agent::notation;

Rational::Rational(int64_t n, int64_t d)
{
    if (d == 0) {
        throw std::invalid_argument("Rational with zero denominator");
    }
    if (d < 0) {
        n = -n;
        d = -d;
    }
    const int64_t g = std::gcd(n < 0 ? -n : n, d);
    num = g ? n / g : 0;
    den = g ? d / g : 1;
}

Rational Rational::operator+(const Rational& o) const { return Rational(num * o.den + o.num * den, den * o.den); }
Rational Rational::operator-(const Rational& o) const { return Rational(num * o.den - o.num * den, den * o.den); }
Rational Rational::operator*(const Rational& o) const { return Rational(num * o.num, den * o.den); }
Rational Rational::operator/(const Rational& o) const { return Rational(num * o.den, den * o.num); }
bool Rational::operator<(const Rational& o) const { return num * o.den < o.num * den; }

std::string Rational::toString() const
{
    return den == 1 ? std::to_string(num) : std::to_string(num) + "/" + std::to_string(den);
}

Rational Duration::value() const
{
    Rational v;
    switch (base) {
    case BaseDuration::Breve: v = Rational(2); break;
    case BaseDuration::Whole: v = Rational(1); break;
    case BaseDuration::Half: v = Rational(1, 2); break;
    case BaseDuration::Quarter: v = Rational(1, 4); break;
    case BaseDuration::Eighth: v = Rational(1, 8); break;
    case BaseDuration::Sixteenth: v = Rational(1, 16); break;
    case BaseDuration::ThirtySecond: v = Rational(1, 32); break;
    case BaseDuration::SixtyFourth: v = Rational(1, 64); break;
    case BaseDuration::OneTwentyEighth: v = Rational(1, 128); break;
    }
    Rational total = v;
    Rational add = v;
    for (int i = 0; i < dots; ++i) {
        add = add * Rational(1, 2);
        total = total + add;
    }
    return total;
}

int Pitch::midi() const
{
    static const int semis[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    return (octave + 1) * 12 + semis[step - 'A'] + alter;
}

bool Event::operator==(const Event& o) const
{
    return kind == o.kind && heads == o.heads
           && (kind == EventKind::FullBarRest || duration == o.duration)
           && marks == o.marks && slurOpen == o.slurOpen && slurClose == o.slurClose
           && graceBeforeKind == o.graceBeforeKind && graceBefore == o.graceBefore && graceAfter == o.graceAfter;
}

bool Tuplet::operator==(const Tuplet& o) const
{
    return actual == o.actual && normal == o.normal && items == o.items && slurOpen == o.slurOpen && slurClose == o.slurClose;
}

Rational mu::agent::notation::itemsDuration(const std::vector<Item>& items, const Rational& barLength)
{
    Rational total;
    for (const Item& item : items) {
        if (const Event* e = std::get_if<Event>(&item)) {
            total = total + (e->kind == EventKind::FullBarRest ? barLength : e->duration.value());
        } else {
            const Tuplet& t = std::get<Tuplet>(item);
            total = total + itemsDuration(t.items, barLength) * Rational(t.normal, t.actual);
        }
    }
    return total;
}
