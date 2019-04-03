//
// Copyright (c) 2018-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/vinniefalco/BeastLounge
//

#include "channel.hpp"
#include "channel_list.hpp"
#include "rpc.hpp"
#include "server.hpp"
#include "service.hpp"
#include "types.hpp"
#include "user.hpp"
#include <boost/beast/_experimental/json/value.hpp>
#include <boost/beast/core/static_string.hpp>
#include <boost/make_unique.hpp>
#include <functional>
#include <vector>
#include <utility>

namespace {

//------------------------------------------------------------------------------

enum class error
{
    not_playing = 1,
    already_playing,
    already_leaving,
    no_open_seat
};

} // (anon)
namespace boost {
namespace system {
template<>
struct is_error_code_enum<error>
{
    static bool constexpr value = true;
};
} // system
} // boost
namespace {

class error_codes : public beast::error_category
{
public:
    const char*
    name() const noexcept override
    {
        return "beast-lounge.blackjack";
    }

    std::string
    message(int ev) const override
    {
        switch(static_cast<error>(ev))
        {
        default:
        case error::not_playing: return
            "Not playing";
        case error::already_playing: return
            "Already playing";
        case error::already_leaving: return
            "Already playing";
        case error::no_open_seat: return
            "No open seat";
        }
    }

    beast::error_condition
    default_error_condition(
        int ev) const noexcept override
    {
        return {ev, *this};
    }
};

beast::error_code
make_error_code(error e)
{
    static error_codes const cat{};
    return {static_cast<std::underlying_type<
        error>::type>(e), cat};
}


//------------------------------------------------------------------------------

class shoe
{
    std::vector<char> cards_;
    std::vector<char>::iterator pos_;

public:
    explicit
    shoe(int decks)
    {
        cards_.resize(decks * 52);
        shuffle();
    }

    void
    shuffle()
    {
        for(std::size_t i = 0;
            i < cards_.size(); ++i)
        {
            cards_[i] = 1 + (i % 52);
        }
        for(std::size_t i = 0;
            i < cards_.size() - 1; ++i)
        {
            auto const j = i +
                (rand() % (cards_.size() - i));
            std::swap(
                cards_[i], cards_[j]);
        }
        pos_ = cards_.begin();
    }

    char
    deal()
    {
        if(pos_ == cards_.end())
            shuffle();
        return *pos_++;
    }
};

//------------------------------------------------------------------------------

struct hand
{
    // cards[0]==0 for hole card
    beast::static_string<22> cards;
    bool busted = false;
    bool twenty_one = false;
    bool blackjack = false;

    void
    clear()
    {
        cards.clear();
        busted = false;
        twenty_one = false;
        blackjack = false;
    }

    // Returns the value of a card,
    // always returns 1 for aces
    static
    int
    value(char c)
    {
        auto v = 1 + ((c - 1) % 13);
        if( v > 10)
            v = 10;
        return v;
    }

    void
    eval()
    {
        int aces = 0;
        int total = 0;
        for(auto c : cards)
        {
            if(c != 1)
            {
                total += c;
            }
            else
            {
                ++aces;
                total += 11;
            }
        }
        while(total > 21 && aces--)
            total -= 10;

        busted = total > 21;
        twenty_one = total == 21;
        blackjack =
            twenty_one &&
            cards.size() == 2;
    }

    void
    hit(shoe& s)
    {
        cards.push_back(s.deal());
    }

    bool
    is_finished() const
    {
        return busted || twenty_one;
    }

    void
    to_json(json::value& jv) const
    {
        auto& arr = jv.emplace_array();
        arr.insert(arr.end(),
            cards.begin(), cards.end());
    }
};

//------------------------------------------------------------------------------

// Represents one of the five seating
// positions of the blackjack playfield.
//
struct seat
{
    enum state_t
    {
        dealer,
        waiting,
        playing,
        leaving,
        open
    };

    user* u = nullptr;
    state_t state = open;
    std::vector<hand> hands;
    int chips;

    seat()
    {
        hands.resize(1);
    }

    void
    to_json(json::value& jv) const
    {
        jv = json::object{};
        jv.emplace("hands", hands);
        switch(state)
        {
        case dealer:
            jv.emplace("state", "dealer");
            break;

        case waiting:
            jv.emplace("state", "waiting");
            jv.emplace("user", u->name);
            jv.emplace("chips", chips);
            break;

        case playing:
            jv.emplace("state", "playing");
            jv.emplace("user", u->name);
            jv.emplace("chips", chips);
            break;

        case leaving:
            jv.emplace("state", "leaving");
            jv.emplace("user", u->name);
            jv.emplace("chips", chips);
            break;

        case open:
            jv.emplace("state", "open");
            break;
        }
    }
};

//------------------------------------------------------------------------------

/*  State machine for a blackjack game.

    Each round evolves through a series of stages

    1.  Place Bets

        All players have a limited time to place valid bets.
        The client defaults to the previous bet if any.
        A player who doesn't bet is ejected from the seat.

    2. Dealing

        The first card is dealt face up to each seat 1 to 5.
        The dealer's first card is dealt face down.
        The second card is dealt face up to each seat 1 to 5.
        The dealer's second card is dealt face down.
*/
enum class stage
{
    // Waiting for players
    wait,

    // Bets being placed
    bet,

    // Cards being dealt
    deal,

    // Players making choices
    play,

    // House hand playing out
    house,

    // Bets paid out or collected
    payout,

    // Deck is being shuffled
    shuffle
};

class game
{
    shoe shoe_;

    // 0    = dealer
    // 1..5 = player
    std::vector<seat> seat_;

    ::stage stage_ = ::stage::wait;

    // which seat's turn
    int turn_ = 1;

    std::size_t
    find(user const& u) const
    {
        for(auto const& s : seat_)
            if( s.state != seat::open &&
                s.u == &u)
                return &s - &seat_.front();
        return 0;
    }

    std::size_t
    insert(user& u)
    {
        for(auto& s : seat_)
            if(s.state == seat::open)
            {
                s.state = seat::waiting;
                s.u = &u;
                return &s - &seat_.front();
            }
        return 0;
    }

public:
    explicit
    game(int decks)
        : shoe_(decks)
    {
        BOOST_ASSERT(
            decks >= 1 && decks <= 6);
        seat_.resize(6);
        seat_[0].state = seat::dealer;
    }

    ::stage
    stage() const noexcept
    {
        return stage_;
    }

    // Join the game as a player.
    // Returns seat assignment on success
    int
    join(
        user& u,
        beast::error_code& ec)
    {
        if(find(u) != 0)
        {
            ec = error::already_playing;
            return 0;
        }
        for(auto& s : seat_)
        {
            if(s.state == seat::open)
            {
                if(stage_ == ::stage::wait)
                {
                    stage_ = ::stage::bet;
                    s.state = seat::playing;
                }
                else
                {
                    s.state = seat::waiting;
                }
                s.u = &u;
                ec.clear();
                return &s - &seat_.front();
            }
        }
        ec = error::no_open_seat;
        return 0;
    }

    // Leave the game as a player.
    //  1 = now leaving, was playing
    //  2 = now open, was waiting
    int
    leave(
        user& u,
        beast::error_code& ec)
    {
        auto const i = find(u);
        if(! i)
        {
            ec = error::not_playing;
            return 0;
        }
        switch(seat_[i].state)
        {
        case seat::waiting:
            seat_[i].state = seat::open;
            return 2;

        case seat::playing:
            seat_[i].state = seat::leaving;
            return 1;
        
        default:
            BOOST_ASSERT(
                seat_[i].state == seat::leaving);
            ec = error::already_leaving;
            return 0;
        }
    }

    // Surrender the hand and leave
    //  1 = success
    // -1 = not playing
    int
    surrender(user& u)
    {
        auto const i = find(u);
        if(! i)
            return -1;
        seat_[i].state = seat::open;
        return 1;
    }

    void
    hit()
    {
        seat_[1].hands[0].hit(shoe_);
    }

    void
    to_json(json::value& jv) const
    {
        auto& obj = jv.emplace_object();
        {
            auto& arr = obj.emplace(
                "seats", json::array{}).first->second;
            for(std::size_t i = 0;
                i < seat_.size(); ++i)
                arr.emplace_back(seat_[i]);
        }
    }
};

//------------------------------------------------------------------------------

class table : public channel
{
    using lock_guard =
        std::lock_guard<std::mutex>;
   
    server& srv_;
    timer_type timer_;
    std::mutex mutable mutex_;
    game g_;

public:
    explicit
    table(server& srv)
        :  channel(
            3,
            "Blackjack",
            srv.channel_list())
        , srv_(srv)
        , timer_(srv.make_executor())
        , g_(1)
    {
        boost::ignore_unused(srv_);
    }

private:
    // Post a call to the strand
    template<class... Args>
    void
    post(Args&&... args)
    {
        net::post(
            timer_.get_executor(),
            beast::bind_front_handler(
                std::forward<Args>(args)...));
    }

    //--------------------------------------------------------------------------
    //
    // channel
    //
    //--------------------------------------------------------------------------

    void
    on_insert(user& u) override
    {
        post(
            &table::do_insert,
            this,
            shared_from(&u));
    }

    void
    on_erase(user& u) override
    {
        post(
            &table::do_erase,
            this,
            std::reference_wrapper<user>(u));
    }

    void
    on_dispatch(rpc_call& rpc) override
    {
        if(rpc.method == "play")
        {
            post(&table::do_play, this, std::move(rpc));
        }
        else if(rpc.method == "watch")
        {
            post(&table::do_watch, this, std::move(rpc));
        }
        else if(rpc.method == "bet")
        {
            post(&table::do_bet, this, std::move(rpc));
        }
        else if(rpc.method == "hit")
        {
            post(&table::do_hit, this, std::move(rpc));
        }
        else if(rpc.method == "stand")
        {
            post(&table::do_stand, this, std::move(rpc));
        }
        else
        {
            rpc.fail(rpc_code::method_not_found);
        }
    }

    //--------------------------------------------------------------------------
    //
    // table
    //
    //--------------------------------------------------------------------------

    void
    update(beast::string_view action)
    {
        json::value jv;
        jv["cid"] = cid();
        jv["verb"] = "update";
        jv["action"] = action;
        jv["game"] = g_;
        send(jv);
    }

    void
    on_timer(beast::error_code ec)
    {
        if(ec == net::error::operation_aborted)
            return;

        if(ec)
        {
            // log error
            BOOST_ASSERT(! ec);
        }


    }

    //--------------------------------------------------------------------------

    void
    do_insert(boost::shared_ptr<user> sp)
    {
        json::value jv;
        jv["cid"] = cid();
        jv["verb"] = "update";
        jv["action"] = "init";
        jv["game"] = g_;
        sp->send(jv);
    }

    void
    do_erase(user& u)
    {
        auto const result = g_.surrender(u);
        if(result == 1)
            update("surrender");
    }

    void
    do_play(rpc_call&& rpc)
    {
        try
        {
            // TODO Optional seat choice
            beast::error_code ec;
            bool const is_wait =
                g_.stage() == stage::wait;
            g_.join(*rpc.u, ec);
            if(ec)
                rpc.fail(ec.message());
            if(is_wait && g_.stage() != stage::wait)
            {
                timer_.expires_after(
                    std::chrono::seconds(10));
                timer_.async_wait(
                    beast::bind_front_handler(
                        &table::on_timer,
                        this));
            }
            update("play");
            rpc.complete();
        }
        catch(rpc_error const& e)
        {
            rpc.complete(e);
        }
    }

    void
    do_watch(rpc_call&& rpc)
    {
        try
        {
            beast::error_code ec;
            g_.leave(*rpc.u, ec);
            if(ec)
                rpc.fail(ec.message());
            update("watch");
            rpc.complete();
        }
        catch(rpc_error const& e)
        {
            rpc.complete(e);
        }
    }

    void
    do_bet(rpc_call&& rpc)
    {
        try
        {
            //g_.bet(rpc.user);
            update("bet");
            rpc.complete();
        }
        catch(rpc_error const& e)
        {
            rpc.complete(e);
        }
    }

    void
    do_hit(rpc_call&& rpc)
    {
        try
        {
            g_.hit();
            update("hit");
            rpc.complete();
        }
        catch(rpc_error const& e)
        {
            rpc.complete(e);
        }
    }

    void
    do_stand(rpc_call&& rpc)
    {
        try
        {
        }
        catch(rpc_error const& e)
        {
            rpc.complete(e);
        }
    }
};

//------------------------------------------------------------------------------

class blackjack_service
    : public service
{
    server& srv_;

public:
    blackjack_service(
        server& srv)
        : srv_(srv)
    {
    }

    //--------------------------------------------------------------------------
    //
    // service
    //
    //--------------------------------------------------------------------------

    void
    on_start() override
    {
        insert<table>(srv_.channel_list(), srv_);
    }

    void
    on_stop() override
    {
    }
};

} // (anon)

//------------------------------------------------------------------------------

void
make_blackjack_service(
    server& srv)
{
    srv.insert(boost::make_unique<blackjack_service>(srv));
}
