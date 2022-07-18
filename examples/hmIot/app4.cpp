/*
 * app4.cpp: command-line application to exercise mbps.hpp
 *
 * This is an application using the mbps shim. Message body is packaged
 * in int8_t vectors are passed between application and mbps. Parameters
 * are passed to mpbs in an vector of pairs (msgParms) along with an optional
 * callback if message qos is desired (confirmation that the message has been published).
 * Parameters are passed from mbps to the application in a mbpsMsg structure that
 * contains an unordered_map where values are indexed by the tags (components of Names)
 * that are defined in the trust schema for this particular application.
 *
 * app4 models an asymmetric, request/response style protocol between controlling
 * agent(s) ("operator" role in the schema) and controlled agent(s) ("device" role
 * in the schema). If the identity bundle gives the app an 'operator' role, it
 * periodically publishes a message and prints all the responses it receives.
 * If the app is given a 'device' role, it waits for a message then sets its
 * simulated state based on the message an announces its current state.
 *
 * app4 is a version of app2 that has been modified for testing relay. It has two
 * differences:
 *  1. It runs forever
 *  2. Rather than throwing an error if tries to build a publication that is not
 *     in its identity bundle, it just ignores it.
 *
 * Copyright (C) 2020-22 Pollere LLC
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere LLC at info@pollere.net.
 *
 *  The DCT proof-of-concept is not intended as production code.
 *  More information on DCT is available from info@pollere.net
 */

#include <getopt.h>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>

#include <dct/shims/mbps.hpp>

using namespace std::literals;

static constexpr bool deliveryConfirmation = false; // get per-message delivery confirmation

// handles command line
static struct option opts[] = {
    {"capability", required_argument, nullptr, 'c'},
    {"debug", no_argument, nullptr, 'd'},
    {"help", no_argument, nullptr, 'h'},
    {"wait", required_argument, nullptr, 'w'}
};
static void usage(const char* cname)
{
    std::cerr << "usage: " << cname << " [flags] id.bundle\n";
}
static void help(const char* cname)
{
    usage(cname);
    std::cerr << " flags:\n"
           "  -c capability     defaults to 'lock'\n"
           "  -d |--debug       enable debugging output\n"
           "  -h |--help        print help then exit\n"
           "  -w |--wait        wait (in ms) between sends\n";
}

/* Globals */
static std::string myPID, myId, role;
static std::chrono::microseconds pubWait = std::chrono::seconds(1);
static int Cnt = 0;
static std::string capability{"lock"};
static std::string myState{"unlocked"};       // simulated state (for devices)

/*
 * msgPubr passes messages to publish to mbps. A simple lambda
 * is used if "qos" is desired. A more complex callback (messageConfirmation)
 * is included in the app1.cpp file.
 */
static void msgPubr(mbps &cm) {
    // make a message to publish
    std::string s = format("Msg #{} from {}:{}-{}", ++Cnt, role, myId, myPID);
    std::vector<uint8_t> toSend(s.begin(), s.end());
    msgParms mp;

    if(role == "operator") {
        std::string a = (std::rand() & 2)? "unlock" : "lock"; // randomly toggle requested action
        std::string l = (std::rand() & 2)? "gate" : "frontdoor"; // randomly toggle targeted location
        mp = msgParms{{"target", capability},{"topic", "command"s},{"trgtLoc",l},{"topicArgs", a}};
        print("{}:{}-{} publishing msg {} targeted at {}\n", role, myId, myPID, Cnt, l);
    } else {
        mp = msgParms{{"target", capability},{"topic", "event"s},{"trgtLoc",myId},{"topicArgs", myState}};
    }

    if constexpr (deliveryConfirmation) {
        try {
            cm.publish(std::move(mp), toSend, [ts=std::chrono::system_clock::now()](bool delivered, uint32_t) {
                    using ticks = std::chrono::duration<double,std::ratio<1,1000000>>;
                    auto now = std::chrono::system_clock::now();
                    auto dt = ticks(now - ts).count() / 1000.;
                    print("{:%M:%S} {}:{}-{} #{} published and {} +{:.3} mS\n",
                            ticks(ts.time_since_epoch()), role, myId, myPID, Cnt,
                            delivered? "confirmed":"timed out", dt);
                    });
        } catch (const std::exception&) {
            std::cout << "    msg " << Cnt << " structure is not permitted for this entity\n";
        }
    } else {
        try {
            cm.publish(std::move(mp), toSend);  //no callback to skip message confirmation
        } catch (const std::exception&) {
            std::cout << "    msg " << Cnt << " structure is not permitted for this entity\n";
        }
    }

    // operators send periodic messages, devices respond to incoming msgs
    if (role == "operator") {
        Cnt++;
        cm.oneTime(pubWait + std::chrono::milliseconds(rand() & 0x1ff), [&cm](){ msgPubr(cm); });
    }
}

/*
 * msgPrnt prints the message received in the subscription
 */
static void msgPrnt(mbps&, const mbpsMsg& mt, std::vector<uint8_t>& msgPayload) {
    using ticks = std::chrono::duration<double,std::ratio<1,1000000>>;
    auto now = std::chrono::system_clock::now();
    auto dt = ticks(now - mt.time("mts")).count() / 1000.;

    print("{:%M:%S} {}:{}-{} rcvd ({:.3} mS transit): {} {}: {} {} | {}\n",
            ticks(now.time_since_epoch()), role, myId, myPID, dt, mt["target"],
                mt["topic"], mt["trgtLoc"], mt["topicArgs"],
                std::string(msgPayload.begin(), msgPayload.end()));
}

/*
 * msgRecv handles a message received in subscription.
 * Used as callback passed to subscribe()
 * The message is opaque to mbps which uses
 * a msgMsg to pass tag data (tags from trust schema)
 *
 * Prints the message content
 * Could take action(s) based on message content
 */

void msgRecv(mbps &cm, const mbpsMsg& mt, std::vector<uint8_t>& msgPayload)
{
    using ticks = std::chrono::duration<double,std::ratio<1,1000000>>;
    auto now = std::chrono::system_clock::now();
    auto dt = ticks(now - mt.time("mts")).count() / 1000.;

    print("{:%M:%S} {}:{}-{} rcvd ({:.3} mS transit): {} {}: {} {} | {}\n",
            ticks(now.time_since_epoch()), role, myId, myPID, dt, mt["target"],
                mt["topic"], mt["trgtLoc"], mt["topicArgs"],
                std::string(msgPayload.begin(), msgPayload.end()));

    // further action can be conditional upon msgArgs and msgPayload

    // devices set their 'state' from the incoming 'arg' value then immediately reply
    if (role == "device") {
        myState = mt["topicArgs"] == "lock"? "locked":"unlocked";
        msgPubr(cm);
    }
}

/*
 * Main() for the application to use.
 * First complete set up: parse input line, set up message to publish,
 * set up entity identifier. Then make the mbps, connect, and run the context.
 */

static int debug = 0;

int main(int argc, char* argv[])
{
    std::srand(std::time(0));
    // parse input line
    for (int c;
        (c = getopt_long(argc, argv, ":c:dhl:n:w:", opts, nullptr)) != -1;) {
        switch (c) {
                case 'c':
                    capability = optarg;
                    break;
                case 'd':
                    ++debug;
                    break;
                case 'h':
                    help(argv[0]);
                    exit(0);
                case 'w':
                    pubWait = std::chrono::milliseconds(std::stoi(optarg));
                    break;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        exit(1);
    }
    myPID = std::to_string(getpid());
    mbps cm(argv[optind]);     //Create mbps
    role = cm.attribute("_role");
    myId = cm.attribute("_roleId");

    if (role == "operator") {
        cm.subscribe(msgRecv);  // single callback for all messages
    } else if (role == "hub") { // role for publisher privacy examples in locateapp
        cm.subscribe(msgPrnt);
    } else {
        //here devices just subscribe to command topic
        cm.subscribe(capability + "/command/" + myId, msgRecv); // msgs to this instance
        cm.subscribe(capability + "/command/all", msgRecv);     // msgs to all instances
    }

    // Connect and pass in the handler
    try {
        /* main task for this entity */
        cm.connect([&cm]{ if (role == "operator") msgPubr(cm); });
    } catch (const std::exception& e) {
        std::cerr << "main encountered exception while trying to connect: " << e.what() << std::endl;
        exit(1);
    } catch (int conn_code) {
        std::cerr << "main mbps failed to connect with code " << conn_code << std::endl;
        exit(1);
    } catch (...) {
        std::cerr << "default exception";
        exit(1);
    }

    cm.run();
}