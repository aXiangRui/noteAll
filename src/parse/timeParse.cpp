#include "parse/timeParse.hpp"

namespace parse
{
    namespace
    {
        // ═══════════════════════════════════════════════════════════════════
        // 小工具
        // ═══════════════════════════════════════════════════════════════════

        // s 的 pos 位置是不是 lit 开头。pos 超界也算"不是"，不抛异常。
        bool at(std::string_view s, std::size_t pos, std::string_view lit)
        {
            return pos <= s.size() && s.substr(pos).starts_with(lit);
        }

        // 冒号的字节长度：半角 ":" 是 1 字节，全角 "："（U+FF1A）是 3 字节。
        // 返回 0 表示这里不是冒号。
        //
        // 为什么必须两种都认：中文输入法下敲时间，十有八九打出的是全角冒号
        // 「8：30」。以前只认半角，于是这类输入**整段时间都解析不出来**，
        // 而且不会报错，只是悄悄留空 —— 用户根本不知道发生了什么。
        std::size_t colonLength(std::string_view s, std::size_t pos)
        {
            if( at(s, pos, ":") )
            {
                return 1;
            }
            if( at(s, pos, "：") )
            {
                return 3;
            }
            return 0;
        }

        // 跳过一段空白。
        //
        // 为什么需要：CLI 的 `note add 明天 下午3点` 会把 argv 用空格拼成
        // 「明天 下午3点」，而解析器原本要求后缀**紧贴**主体。不做这一步，
        // 用户按最自然的方式敲命令，时刻就丢了。
        //
        // 只跳 ASCII 空白。全角空格（U+3000）不跳 —— 它更可能是正文的一部分。
        std::size_t skipSpaces(std::string_view s, std::size_t pos)
        {
            while( pos < s.size() && (s[pos] == ' ' || s[pos] == '\t') )
            {
                ++pos;
            }
            return pos;
        }

        // 读十进制数字，返回 (值, 吃掉几个字节)。
        // 读到的位数少于 minDigits 就失败 —— 也顺手挡住了空串和"根本没数字"。
        std::optional<std::pair<int, std::size_t>> readNumber(std::string_view s, std::size_t minDigits, std::size_t maxDigits)
        {
            std::size_t n = 0;
            int         v = 0;
            while( n < maxDigits && n < s.size() && s[n] >= '0' && s[n] <= '9' )
            {
                v = v * 10 + (s[n] - '0');
                ++n;
            }
            if( n < minDigits )
            {
                return std::nullopt;
            }
            return std::make_pair(v, n);
        }

        // 中文数字：一 二 两 三 … 十 十一 … 二十 … 五十九
        //
        // 只服务「钟点」这一处（三点 / 十点半 / 十点十五分），**不碰日期** ——
        // 「二十号」「三月五号」仍然不支持（缺月份的中文日期歧义大，见 parse-time.md §3.5）。
        //
        // 返回 (值, 吃掉几个字节)。中文数字一个字 3 字节，所以长度按 word.size() 累加，
        // 别写死 1 —— 那是按"字符数"算，UTF-8 下会错位。
        std::optional<std::pair<int, std::size_t>> readChineseNumber(std::string_view s)
        {
            struct Word
            {
                std::string_view text_;
                int              value_;
            };

            static constexpr Word UNITS[] = {
                { "零", 0 }, { "〇", 0 }, { "一", 1 }, { "二", 2 }, { "两", 2 }, { "三", 3 },
                { "四", 4 }, { "五", 5 }, { "六", 6 }, { "七", 7 }, { "八", 8 }, { "九", 9 },
            };

            // 在 s 的开头读一个「个位字」。
            const auto takeUnit = [](std::string_view rest) -> std::optional<std::pair<int, std::size_t>>
            {
                for( const auto& w : UNITS )
                {
                    if( rest.starts_with(w.text_) )
                    {
                        return std::make_pair(w.value_, w.text_.size());
                    }
                }
                return std::nullopt;
            };

            constexpr std::size_t TEN_LEN = 3;   // "十" 的字节数

            // 形如 十 / 十一 / 十三
            if( s.starts_with("十") )
            {
                std::size_t used  = TEN_LEN;
                int         value = 10;

                if( const auto unit = takeUnit(s.substr(used)) )
                {
                    value += unit->first;
                    used  += unit->second;
                }

                if( value > 59 )
                {
                    return std::nullopt;
                }
                return std::make_pair(value, used);
            }

            const auto first = takeUnit(s);
            if( !first )
            {
                return std::nullopt;
            }

            std::size_t used  = first->second;
            int         value = first->first;

            // 零五 / 〇五：钟点里的「零五分」读作 5（口语里 10:05 就这么说）
            if( value == 0 )
            {
                if( const auto next = takeUnit(s.substr(used)) )
                {
                    return std::make_pair(next->first, used + next->second);
                }
                return std::make_pair(0, used);
            }

            // 形如 二十 / 二十三
            if( s.substr(used).starts_with("十") )
            {
                used  += TEN_LEN;
                value  = first->first * 10;

                if( const auto unit = takeUnit(s.substr(used)) )
                {
                    value += unit->first;
                    used  += unit->second;
                }
            }

            if( value > 59 )
            {
                return std::nullopt;
            }
            return std::make_pair(value, used);
        }

        // 钟点里的数：先试阿拉伯数字（15点 / 9点半），再试中文数字（三点 / 十点半）。
        std::optional<std::pair<int, std::size_t>> readClockNumber(std::string_view s)
        {
            if( const auto arabic = readNumber(s, 1, 2) )
            {
                return arabic;
            }
            return readChineseNumber(s);
        }

        // ═══════════════════════════════════════════════════════════════════
        // 后缀（时刻）
        // ═══════════════════════════════════════════════════════════════════

        struct ClockHit
        {
            std::chrono::minutes sinceMidnight_;
            int                  dayOffset_;   // 0 / 1：「晚上12点」是次日零点，不是当天中午
            std::size_t          length_;
        };

        struct ClockRange
        {
            std::chrono::minutes                start_;
            std::optional<std::chrono::minutes> end_;            // 空 = 时间点；非空 = 时段
            std::size_t                         length_;
            int                                 startDayOffset_;  // 语义同 ClockHit::dayOffset_
            int                                 endDayOffset_;
        };

        // 三个构造函数。字段排在一起之后，位置初始化很容易把 length_ 和 dayOffset_
        // 写反（两个都是整数，编译器一声不吭），所以统一从这里出。
        ClockRange pointRange(std::chrono::minutes start, std::size_t length)
        {
            return ClockRange{ start, std::nullopt, length, 0, 0 };
        }

        ClockRange pointHit(const ClockHit& h, std::size_t length)
        {
            return ClockRange{ h.sinceMidnight_, std::nullopt, length, h.dayOffset_, 0 };
        }

        ClockRange spanRange(const ClockHit& a, const ClockHit& b, std::size_t length)
        {
            return ClockRange{ a.sinceMidnight_, b.sinceMidnight_, length,
                               a.dayOffset_, b.dayOffset_ };
        }

        // 钟点相对当天零点的偏移（分钟），跨天的那一天也算进去。
        // 比两个钟点谁早谁晚必须用它：光看 sinceMidnight_，
        // 「晚上11点到12点半」的次日 00:30 会被当成比 23:00 还早。
        std::chrono::minutes offsetOf(const ClockHit& h)
        {
            return h.sinceMidnight_ + std::chrono::days{ h.dayOffset_ };
        }

        // 时段词对钟点的修正结果：改完的小时 + 要不要顺延一天。
        //
        // ⚠️ 每个时段词的"有效钟点"不一样，不能拿"小于 12 就 +12"一把梭：
        //     中午11点 / 中午12点             -> 11:00 / 12:00（照字面）
        //     中午1点 / 中午2点               -> 13:00 / 14:00
        //     中午9点                         -> 没这个说法，返回空
        //     下午12点 / 上午12点             -> 12:00
        //     凌晨12点                        -> 00:00（当天，凌晨是后半夜）
        //     晚上6点..11点                   -> 18:00..23:00
        //     晚上1点..5点                    -> 次日 01:00..05:00
        //     晚上12点                        -> 次日 00:00（这一天的 24 点）
        //     晚上0点 / 0点                   -> 00:00（当天）
        //
        // 为什么返回 optional：以前「中午9点」被算成 21:00 —— 又一个不报错的
        // 12 小时偏移。中午的钟点只可能是 11、12、1、2，写别的不是"猜个近似"，
        // 而是根本没这个说法，交给调用方整段不认。
        struct HourFix
        {
            int hour_;
            int dayOffset_;
        };

        std::optional<HourFix> applyPeriod(int hour, std::string_view period)
        {
            if( period == "晚上" )
            {
                if( hour == 0 )  { return HourFix{ 0, 0 }; }         // 晚上0点 = 当天零点
                if( hour == 12 ) { return HourFix{ 0, 1 }; }         // 晚上12点 = 次日零点
                if( hour >= 13 ) { return HourFix{ hour, 0 }; }      // 已经是 24 小时制
                if( hour >= 6 )  { return HourFix{ hour + 12, 0 }; } // 晚上6..11点 = 18..23
                return HourFix{ hour, 1 };                           // 晚上1..5点 = 次日凌晨
            }
            if( period == "中午" )
            {
                if( hour == 11 || hour == 12 ) { return HourFix{ hour, 0 }; }
                if( hour == 1 || hour == 2 )   { return HourFix{ hour + 12, 0 }; }
                return std::nullopt;                                 // 中午9点这种没这个说法
            }
            if( period == "下午" )
            {
                return HourFix{ hour < 12 ? hour + 12 : hour, 0 };
            }
            if( period == "凌晨" )
            {
                return HourFix{ hour == 12 ? 0 : hour, 0 };          // 凌晨12点 = 00:00
            }
            return HourFix{ hour, 0 };   // 早上 / 上午 / 没写时段词：照字面
        }

        // 只试钟点本身，不试时段词。period 可以是空的 —— 空表示前面没写时段词。
        std::optional<ClockHit> matchClockNumber(std::string_view s, std::string_view period)
        {
            // 形式甲：15:30 / 15：30（全角冒号）
            if( const auto h = readNumber(s, 1, 2); h && colonLength(s, h->second) > 0 )
            {
                const std::size_t minutePos = h->second + colonLength(s, h->second);
                const auto        m         = readNumber(s.substr(minutePos), 1, 2);
                if( !m || h->first > 23 || m->first > 59 )
                {
                    return std::nullopt;
                }
                // ⚠️ 这里也必须走 applyPeriod：「下午3:30」是 15:30，跟「下午3点半」一样。
                // 这个分支以前直接 return，于是同一个意思两种写法差 12 小时，还不报错。
                const auto fix = applyPeriod(h->first, period);
                if( !fix || fix->hour_ > 23 )
                {
                    return std::nullopt;
                }
                return ClockHit{ std::chrono::minutes{ fix->hour_ * 60 + m->first },
                                 fix->dayOffset_, minutePos + m->second };
            }

            // 形式乙：15点 / 9点半 / 15点30分 / 三点 / 十点半
            const auto h = readClockNumber(s);
            if( !h || !at(s, h->second, "点") )     // 「点」占 3 字节
            {
                return std::nullopt;
            }

            std::size_t          pos   = h->second + 3;
            std::chrono::minutes extra{ 0 };

            if( at(s, pos, "半") )
            {
                extra = std::chrono::minutes{ 30 };
                pos  += 3;
            }
            else if( at(s, pos, "一刻") )
            {
                extra = std::chrono::minutes{ 15 };
                pos  += 6;   // 「一刻」6 字节
            }
            else if( at(s, pos, "三刻") )
            {
                extra = std::chrono::minutes{ 45 };
                pos  += 6;
            }
            else if( const auto m = readClockNumber(s.substr(pos)); m && m->first <= 59 )
            {
                // 「分」可有可无：五点四十分 / 五点四十 / 5点40 / 23点50 都认。
                //
                // 以前只认带「分」的，于是「五点四十」被拆成「五点」+ 剩下「四十」，
                // 时间悄悄变成 05:00 —— **静默给出错误时间**比不解析更糟。
                extra = std::chrono::minutes{ m->first };
                pos  += m->second;

                if( at(s, pos, "分") )
                {
                    pos += 3;
                }
            }

            const auto fix = applyPeriod(h->first, period);
            if( !fix || fix->hour_ > 23 )
            {
                return std::nullopt;
            }
            return ClockHit{ std::chrono::minutes{ fix->hour_ * 60 } + extra, fix->dayOffset_, pos };
        }

        // 开头的时段词（可选）。命中就返回词，长度写进 length。
        std::string_view takePeriod(std::string_view s, std::size_t& length)
        {
            for( const auto& p : backWords )
            {
                if( s.starts_with(p.word_) )
                {
                    length = p.word_.size();
                    return p.word_;
                }
            }
            length = 0;
            return {};
        }

        // [时段词]? [钟点]? [到|至 [时段词]? 钟点]?
        // 至少得命中一样，否则返回空（= 这里没有时刻信息）。
        //
        // impliedPeriod 是前面日期那一段留下的隐含时段（「昨晚」→「晚上」）。
        // 文字里明写了时段词就以文字为准，没写才用它。
        std::optional<ClockRange> matchClockRange(std::string_view s, std::string_view impliedPeriod)
        {
            // 开头的时段词，可选
            std::size_t      pos    = 0;
            std::string_view period = takePeriod(s, pos);
            if( period.empty() )
            {
                period = impliedPeriod;
            }

            const auto first = matchClockNumber(s.substr(pos), period);

            if( !first )
            {
                if( period.empty() )
                {
                    return std::nullopt;      // 既没时段词也没钟点
                }

                // 后面紧跟数字 = 用户明明写了钟点，只是它不合法（「中午9点」「下午25点」）。
                // 这时不能只吃掉时段词、把它当成「下午」单独出现 —— 整段不认。
                // 隐含时段（「昨晚」）不走这条：那时 pos 是 0，说明原文里根本没写时段词。
                if( pos > 0 && pos < s.size() && s[pos] >= '0' && s[pos] <= '9' )
                {
                    return std::nullopt;
                }

                // 光有「下午」这种：取它的代表时刻
                for( const auto& p : backWords )
                {
                    if( p.word_ == period )
                    {
                        return pointRange(
                            std::chrono::duration_cast<std::chrono::minutes>(p.fromMidnight_), pos);
                    }
                }
                return std::nullopt;
            }

            pos += first->length_;

            // 「下午3点到5点」——第二个钟点没写时段词就沿用同一个，所以那个 5点 也算 17:00。
            // ⚠️ 它自己写了就用自己的：「晚上11点到凌晨1点」的 1点 是 01:00，不是 13:00。
            //
            // 分隔符除了「到/至」，也认波浪号：`8:30~10:30`、`8:30～10:30`（全角）。
            // 后者是中文输入法下最顺手的写法，和全角冒号是同一类坑。
            for( const std::string_view sep : { "到", "至", "~", "～" } )
            {
                if( !at(s, pos, sep) )
                {
                    continue;
                }

                const std::size_t secondStart = pos + sep.size();
                std::size_t       secondSkip  = 0;
                const std::string_view written = takePeriod(s.substr(secondStart), secondSkip);
                const std::string_view secondPeriod = written.empty() ? period : written;

                auto second = matchClockNumber(s.substr(secondStart + secondSkip), secondPeriod);
                if( !second )
                {
                    continue;
                }

                // 第二个钟点比起点还早，分两种情形处理。
                //
                // ⚠️ 比大小必须带上 dayOffset_：「晚上11点半到12点半」的 12点半 是
                // 次日 00:30（偏移 1470 分钟），光看 sinceMidnight_（30）会以为它更早，
                // 回落成当天 12:30，区间直接歪掉 12 小时。
                if( written.empty() && offsetOf(*second) < offsetOf(*first) )
                {
                    if( !period.empty() )
                    {
                        // 「晚上11点到1点」：第二个钟点没写时段词，沿用「晚上」会把 1点
                        // 读成 13:00。它比起点还早，只能靠跨天补救，跨度一下变成 14 小时
                        // —— 显然不是这个意思。于是改按字面读（1点 = 01:00），跨天之后只剩 2 小时。
                        //
                        // 「下午3点到5点」不受影响：17:00 本来就晚于 15:00，压根不需要跨天。
                        if( const auto literal = matchClockNumber(s.substr(secondStart + secondSkip), {}) )
                        {
                            if( offsetOf(*literal) < offsetOf(*first) )
                            {
                                second = literal;
                            }
                        }
                    }
                    else
                    {
                        // 两边都没写时段词：「11点到1点」是 11:00–13:00，不是跨零点到
                        // 次日 01:00（14 小时）。先在当天补 12 小时 —— 补完晚于起点就用它；
                        // 补完还早（或超过 24 点）才交给下面的"终点早于起点就跨天"。
                        const auto pm = second->sinceMidnight_ + std::chrono::hours{ 12 };
                        if( pm > offsetOf(*first) && pm < std::chrono::hours{ 24 } )
                        {
                            second->sinceMidnight_ = pm;
                            second->dayOffset_     = 0;
                        }
                    }
                }

                return spanRange(*first, *second, secondStart + secondSkip + second->length_);
            }

            // ⚠️ 长度用 pos，不要用 first->length_ ——
            // 前面的时段词（「下午」那 6 个字节）也是被吃掉的。
            return pointHit(*first, pos);
        }

        // ═══════════════════════════════════════════════════════════════════
        // 主体（日期）
        // ═══════════════════════════════════════════════════════════════════

        struct DateHit
        {
            std::chrono::sys_days day_;
            std::size_t           length_;
            std::string_view      impliedPeriod_{};   // 「昨晚」这种自带时段的词才填
            // 年份是写出来的（含 今年 / 明年 / 去年），还是 pickYear 按"还没到的那个"猜的。
            // 只有猜出来的年份才需要防"被推到下一年"（见 parseTime 里的区间检查）。
            bool                  yearExplicit_{ true };
        };

        // 前缀 → 周 / 月的偏移。只有这四个搭得上；前 / 后 / 昨 / 今 / 明 / 去 返回空。
        std::optional<int> prefixOffset(std::string_view pre)
        {
            if(      pre == "上" )                { return -1; }
            else if( pre == "下" )                { return  1; }
            else if( pre == "这" || pre == "本" ) { return  0; }
            return std::nullopt;
        }

        // s 开头那一段是「周」字后面那几个字。命中就返回表里那一项，否则 nullptr。
        // 返回表项而不是只返回 weekday，是因为调用方还得知道它吃掉了几个字节。
        //
        // 表本身在 timeParse.hpp，和另外几张表排在一起；这个查询只在这儿用，
        // 所以关在匿名 namespace 里，外面看不见。
        const WeekdayWord* matchWeekdayWord(std::string_view s)
        {
            for( const auto& w : weekdayWords )
            {
                if( !s.starts_with(w.word_) )
                {
                    continue;
                }
                // 「天」＝周日（口语写法），但「天气」不是星期词：
                // 「下周天气不错」必须一个都不认，不能读成「下周日」+「气不错」。
                if( w.word_ == "天" && at(s, w.word_.size(), "气") )
                {
                    continue;
                }
                return &w;
            }
            return nullptr;
        }

        // 本周的那个星期几（零点）。
        //
        // 注意：不能写 wd - weekday{ baseDay } —— weekday 相减返回的是 [0,6] 的
        // "模 7"结果，不是有符号差：周一(1) 减 周四(4) 得到的是 4 而不是 -3。
        // 用它算会整整偏一周，而且只有目标星期早于基准星期时才出错，
        // 一半用例照过，非常难查。
        std::chrono::sys_days dayOfThisWeek(std::chrono::sys_days baseDay, std::chrono::weekday wd)
        {
            const int offset = (int)wd.iso_encoding()
                             - (int)std::chrono::weekday{ baseDay }.iso_encoding();   // -6 .. 6
            return baseDay + std::chrono::days{ offset };
        }

        struct WeekHit
        {
            std::chrono::sys_days dayOfThisWeek_;   // 本周的那个周X（零点）
            std::size_t           length_;          // 「周X」一共吃掉几个字节
        };

        // 匹配一整条「[周|星期|礼拜] + [一..日]」。
        //
        // 单位词只走 weekUnits（三个）—— 月/年/天/日 绝不能进来，
        // 否则会把「下月一号」当成「下周的周一」。
        std::optional<WeekHit> matchWeekDay(std::string_view s, std::chrono::sys_days baseDay)
        {
            for( const std::string_view unit : weekUnits )
            {
                if( !s.starts_with(unit) )
                {
                    continue;
                }
                const WeekdayWord* wdWord = matchWeekdayWord(s.substr(unit.size()));
                if( wdWord == nullptr )
                {
                    continue;
                }
                return WeekHit{ dayOfThisWeek(baseDay, wdWord->wd_),
                                unit.size() + wdWord->word_.size() };
            }
            return std::nullopt;
        }

        std::optional<std::chrono::sys_days> makeDate(int y, unsigned m, unsigned d)
        {
            const std::chrono::year_month_day ymd{ std::chrono::year{ y }
                                                 / std::chrono::month{ m }
                                                 / std::chrono::day{ d } };
            if( !ymd.ok() )
            {
                return std::nullopt;      // 2月30日 这种
            }
            return std::chrono::sys_days{ ymd };
        }

        // 没写年份时取"还没到的那个"：基准年这个日子还没到就用基准年，过了就用下一年。
        // 今年压根没这一天（2月29日）就往后找 —— 闰年间隔不会超过 8 年。
        //
        // floorDay 是"不许早于哪一天"。单独解析一个日期时它就是基准日；
        // 解析「X 到 Y」里的 Y 时它是 X —— 否则「周一到周五」的周五会留在本周、
        // 周一却跳到下周，区间直接倒过来。
        std::optional<std::chrono::sys_days> pickYear(unsigned m, unsigned d, int baseYear,
                                                     std::chrono::sys_days floorDay)
        {
            for( int offset = 0; offset < 8; ++offset )
            {
                if( const auto t = makeDate(baseYear + offset, m, d); t && *t >= floorDay )
                {
                    return t;
                }
            }
            return std::nullopt;
        }

        // 绝对日期：2026-10-01 或 [2026年 | 今年 | 明年 | 去年]10月1日 或 10月1号
        std::optional<DateHit> matchAbsoluteDate(std::string_view s, std::chrono::sys_days baseDay,
                                                 std::chrono::sys_days floorDay)
        {
            const int baseYear = (int)std::chrono::year_month_day{ baseDay }.year();

            // 形式甲：2026-10-01
            if( const auto y = readNumber(s, 4, 4); y && at(s, 4, "-") )
            {
                const auto mo = readNumber(s.substr(5), 1, 2);
                if( !mo || !at(s, 5 + mo->second, "-") )
                {
                    return std::nullopt;
                }
                const std::size_t dayPos = 5 + mo->second + 1;
                const auto        d      = readNumber(s.substr(dayPos), 1, 2);
                if( !d )
                {
                    return std::nullopt;
                }
                if( const auto t = makeDate(y->first, (unsigned)mo->first, (unsigned)d->first) )
                {
                    return DateHit{ *t, dayPos + d->second };
                }
                return std::nullopt;      // 2026-02-30：写出来了但不合法，不猜
            }

            // 形式乙：[2026年 | 今年 | 明年 | 去年]10月1日
            std::size_t        pos = 0;
            std::optional<int> year;
            if( const auto y = readNumber(s, 4, 4); y && at(s, 4, "年") )
            {
                year = y->first;
                pos  = 4 + 3;             // 「年」占 3 字节
            }
            else
            {
                // 「今年」这种整词已经把「年」吃掉了，所以底下直接从「X月」接着读。
                for( const auto& w : yearWords )
                {
                    if( s.starts_with(w.word_) )
                    {
                        year = baseYear + w.offset_;
                        pos  = w.word_.size();
                        break;
                    }
                }
            }

            const auto mo = readNumber(s.substr(pos), 1, 2);
            if( !mo || !at(s, pos + mo->second, "月") )
            {
                return std::nullopt;
            }
            pos += mo->second + 3;

            const auto d = readNumber(s.substr(pos), 1, 2);
            if( !d )
            {
                return std::nullopt;
            }
            pos += d->second;

            if( at(s, pos, "日") || at(s, pos, "号") )
            {
                pos += 3;
            }
            else
            {
                return std::nullopt;      // 光有「3月5」不算日期
            }

            if( year )
            {
                if( const auto t = makeDate(*year, (unsigned)mo->first, (unsigned)d->first) )
                {
                    return DateHit{ *t, pos };
                }
                return std::nullopt;
            }
            if( const auto t = pickYear((unsigned)mo->first, (unsigned)d->first, baseYear, floorDay) )
            {
                return DateHit{ *t, pos, {}, false };   // 年份是猜的
            }
            return std::nullopt;
        }

        // 光写「今年 / 明年 / 去年」= 那一年的 1 月 1 日。
        // 整年是个区间，M1 这里先只给一个点，跟「月初」一个路数（不填 end）。
        //
        // 后面紧跟数字就不认：「今年10月」缺了「日 / 号」，
        // 宁可整段不解析（和「2026年10月」一致），也不许只吃掉「今年」留下半截。
        std::optional<DateHit> matchYearStart(std::string_view s, std::chrono::sys_days baseDay)
        {
            const int baseYear = (int)std::chrono::year_month_day{ baseDay }.year();

            for( const auto& w : yearWords )
            {
                if( !s.starts_with(w.word_) )
                {
                    continue;
                }
                const std::string_view after = s.substr(w.word_.size());
                if( !after.empty() && after[0] >= '0' && after[0] <= '9' )
                {
                    continue;
                }
                if( const auto t = makeDate(baseYear + w.offset_, 1u, 1u) )
                {
                    return DateHit{ *t, w.word_.size() };
                }
            }
            return std::nullopt;
        }

        // 「[上|下|这|本] + [个]月初」→ 那个月 1 号零点。
        // 想落地成"整月"是个时段（要填 end），M1 先只给一个点。
        std::optional<DateHit> matchMonthStart(std::string_view s, std::chrono::sys_days baseDay)
        {
            for( const std::string_view pre : frontWords )
            {
                const auto monthOffset = prefixOffset(pre);
                if( !monthOffset || !s.starts_with(pre) )
                {
                    continue;
                }

                std::size_t      length = pre.size();
                std::string_view after  = s.substr(length);

                if( after.starts_with("个") )      // 「下个月初」里的那个「个」
                {
                    length += 3;
                    after   = after.substr(3);
                }
                if( !after.starts_with("月初") )
                {
                    continue;
                }

                const std::chrono::year_month_day base{ baseDay };
                const std::chrono::year_month    ym =
                    base.year() / base.month() + std::chrono::months{ *monthOffset };

                return DateHit{ std::chrono::sys_days{ ym / std::chrono::day{ 1 } }, length + 6 };
            }
            return std::nullopt;
        }

        // 各种日期形状依次试。顺序就是优先级。
        //
        // floorDay = "不许早于哪一天"，只影响"取还没到的那个"这类相对规则
        // （裸周X / 裸周末 / 没写年份的月日）。单独解析一个日期时它等于 baseDay；
        // 解析「X 到 Y」里的 Y 时它是 X —— 不这么做，「周一到周五」的周一跳到下周、
        // 周五却留在本周，区间会倒过来。
        std::optional<DateHit> matchDate(std::string_view s, std::chrono::sys_days baseDay,
                                         std::chrono::sys_days floorDay)
        {
            // 形状 1：整词 今天 / 昨天 / 明天 / 后天 / 前天 / 大前天 / 大后天 / 昨晚
            for( const auto& d : simpleWords )
            {
                if( s.starts_with(d.word_) )
                {
                    return DateHit{ baseDay + std::chrono::days{ d.timeOperation_ },
                                    d.word_.size(), d.impliedPeriod_ };
                }
            }

            // 形状 2：[上|下|这|本] + ( 周X | 周末 )
            for( const std::string_view pre : frontWords )
            {
                const auto weekOffset = prefixOffset(pre);
                if( !weekOffset || !s.starts_with(pre) )
                {
                    continue;
                }

                // 「下下周三」「上上周三」＝ 同一个前缀写两遍，偏移累加。
                // 只有 上 / 下 允许叠；「这这周三」不成话，重复也只当一次。
                std::size_t      repeat = 0;
                std::string_view after  = s;
                while( after.starts_with(pre) && repeat < (*weekOffset == 0 ? 1u : 2u) )
                {
                    after.remove_prefix(pre.size());
                    ++repeat;
                }
                const int offset = *weekOffset * (int)repeat;

                if( const auto hit = matchWeekDay(after, baseDay) )
                {
                    return DateHit{ hit->dayOfThisWeek_ + std::chrono::days{ 7 * offset },
                                    pre.size() * repeat + hit->length_ };
                }
                if( after.starts_with("周末") )
                {
                    // 「周末」落在那周的周六。「上周三」是周三，「上周末」就是周六。
                    return DateHit{ dayOfThisWeek(baseDay, std::chrono::Saturday)
                                        + std::chrono::days{ 7 * offset },
                                    pre.size() * repeat + 6 };
                }
            }

            // 形状 3：裸写「周三」= 还没到的那个。
            // 今天正好就是周三就取今天 —— 所以是 `<` 而不是 `<=`。
            if( const auto hit = matchWeekDay(s, baseDay) )
            {
                std::chrono::sys_days target = hit->dayOfThisWeek_;
                if( target < floorDay )
                {
                    target += std::chrono::days{ 7 };
                }
                return DateHit{ target, hit->length_ };
            }

            // 形状 3b：裸写「周末」，同样取还没到的那个
            if( s.starts_with("周末") )
            {
                std::chrono::sys_days target = dayOfThisWeek(baseDay, std::chrono::Saturday);
                if( target < floorDay )
                {
                    target += std::chrono::days{ 7 };
                }
                return DateHit{ target, 6 };
            }

            // 形状 4：绝对日期
            if( const auto absolute = matchAbsoluteDate(s, baseDay, floorDay) )
            {
                return absolute;
            }

            // 形状 5：[上|下|这|本] + [个]月初
            if( const auto monthStart = matchMonthStart(s, baseDay) )
            {
                return monthStart;
            }

            // 形状 6：今年 / 明年 / 去年 -> 那一年的 1 月 1 日
            return matchYearStart(s, baseDay);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 主体 + 后缀 → 合并
    // ═══════════════════════════════════════════════════════════════════════

    ParseResult parseTime(std::string_view raw_text, std::chrono::sys_seconds base)
    {
        const std::chrono::sys_days baseDay = std::chrono::floor<std::chrono::days>(base);

        // ── 主体：先试单个日期，再试「日期 到 日期」 ──
        const auto date = matchDate(raw_text, baseDay, baseDay);

        std::size_t                          pos = date ? date->length_ : 0;
        std::optional<std::chrono::sys_days> dateEnd;

        if( date )
        {
            for( const std::string_view sep : { "到", "至", "~", "～" } )
            {
                // 「周一 到 周五」也认：CLI 拼参数时会在「到」两边留空格。
                const std::size_t sepPos = skipSpaces(raw_text, pos);
                if( !at(raw_text, sepPos, sep) )
                {
                    continue;
                }
                const std::size_t secondStart = skipSpaces(raw_text, sepPos + sep.size());
                // 第二个日期的"不许早于"是第一个日期，否则「周一到周五」会倒过来。
                // 万一对出来的日期仍然更早（「明天到昨天」这种），就整个不吃这段区间 ——
                // 宁可退回"单个日期 + 原文留在剩余里"，也不产出 end < start 的区间。
                const auto second = matchDate(raw_text.substr(secondStart), baseDay, date->day_);
                if( !second || second->day_ < date->day_ )
                {
                    continue;
                }

                // 没写年份的日期，pickYear 会把它推到"还没到的那个"，可能整整推一年。
                // 「3月5日到3月1日」于是变成跨一年的区间 —— 那是口误，不是跨年。
                // 只在三件事同时成立时判为无效：年份是猜的、确实推到了下一年、跨度超过半年。
                // 这样「12月31日到1月1日」（1 天）和写了年份的长区间都照常放行。
                if( !second->yearExplicit_
                    && std::chrono::year_month_day{ second->day_ }.year()
                       > std::chrono::year_month_day{ date->day_ }.year()
                    && second->day_ - date->day_ > std::chrono::days{ 183 } )
                {
                    continue;
                }

                dateEnd = second->day_;
                pos     = secondStart + second->length_;
                break;
            }
        }

        // ── 后缀：时刻（可以带自己的时段词，也可以是个区间）──
        //
        // 主体和时刻之间允许有空格：「明天 下午3点」。这是 CLI 拼接 argv
        // 的必然产物；跳过的空格算进被吃掉的区间，不进 remainder。
        const std::size_t clockPos = skipSpaces(raw_text, pos);
        const auto clock = matchClockRange(raw_text.substr(clockPos),
                                           date ? date->impliedPeriod_ : std::string_view{});
        if( clock )
        {
            pos = clockPos + clock->length_;
        }

        // 两段都没命中 = 这里根本没有时间信息。一个字都不许动。
        if( !date && !clock )
        {
            return ParseResult{ std::nullopt, std::string{ raw_text } };
        }

        // 缺哪段补哪段：没写日期就用基准那天，没写时刻就用零点。
        const std::chrono::sys_days day     = date ? date->day_ : baseDay;
        const std::chrono::sys_days lastDay = dateEnd ? *dateEnd : day;
        const std::chrono::minutes  at      = clock ? clock->start_ : std::chrono::minutes{ 0 };

        // 「晚上12点」这类落在次日的时刻靠 dayOffset_ 把日期推一天。
        const int startDayOffset = clock ? clock->startDayOffset_ : 0;

        // 起点：
        //   没有日期区间 -> 这一天 + 钟点（「周三下午3点」= 周三 15:00）
        //   有日期区间   -> 首日的零点（「周一到周五下午3点」= 周一 00:00 起）
        //
        // 日期 + 钟点同时出现时会有语义二重性（可能是"一条跨度"，也可能是
        // "每天重复"）。这里统一按「一条跨度」处理：起点取首日 00:00，
        // 终点取下一天 + 钟点终点。重复语义留给以后的数据模型，不在解析层猜。
        const std::chrono::sys_seconds clockStart{ day + at
                                                 + std::chrono::days{ startDayOffset } };
        const std::chrono::sys_seconds start = dateEnd
                                             ? std::chrono::sys_seconds{ day }
                                             : clockStart;

        std::optional<std::chrono::sys_seconds> end;

        if( clock && clock->end_ )
        {
            // 终点落在**最后一天**上：「周一到周五下午3点到5点」= 周五 17:00。
            std::chrono::sys_seconds e{ lastDay + *clock->end_
                                      + std::chrono::days{ clock->endDayOffset_ } };

            // 「23点到1点」：终点早于起点就是跨了零点，往后推一天。
            // 「9点到9点」这种正好相等的原样留着，不擅自补成 24 小时。
            //
            // 比较基准是**最后一天**的钟点起点，不是整个区间的起点 ——
            // 否则「周一到周五的23点到1点」里，终点永远大于周一 0 点，
            // 跨天判断就再也不会触发了。
            const std::chrono::sys_seconds lastClockStart{ lastDay + at
                                                         + std::chrono::days{ startDayOffset } };
            if( e < lastClockStart )
            {
                e += std::chrono::days{ 1 };
            }
            end = e;
        }
        else if( dateEnd && clock )
        {
            // 日期区间 + 钟点（点）：「周一到周五下午3点」= 周一 0:00 → 周五 15:00。
            end = std::chrono::sys_seconds{ lastDay + at
                                          + std::chrono::days{ startDayOffset } };
        }
        else if( dateEnd )
        {
            // 纯日期区间：「周一到周五」= 周一 0:00 → 周六 0:00（含周五整天）。
            end = std::chrono::sys_seconds{ *dateEnd } + std::chrono::days{ 1 };
        }

        core::TimeRange range{ start, end };
        return ParseResult{ range, std::string{ raw_text.substr(pos) } };
    }
}
