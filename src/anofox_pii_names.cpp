// Given-name dictionary of the NAME fallback (see anofox_pii_names.hpp).
// Data only: the matching logic lives in NameRecognizer (anofox_pii.cpp).

#include "anofox_pii_names.hpp"

namespace duckdb {
namespace anofox {

const std::unordered_set<std::string> &CommonGivenNames() {
    static const std::unordered_set<std::string> names = {
        // ---- US/UK, male ----
        "james", "john", "robert", "michael", "david", "william", "richard", "joseph",
        "thomas", "charles", "christopher", "daniel", "matthew", "anthony", "mark",
        "donald", "steven", "paul", "andrew", "joshua", "kenneth", "kevin", "brian",
        "george", "timothy", "ronald", "edward", "jason", "jeffrey", "ryan", "jacob",
        "gary", "nicholas", "eric", "jonathan", "stephen", "larry", "justin", "scott",
        "brandon", "benjamin", "samuel", "raymond", "gregory", "frank", "alexander",
        "patrick", "jack", "dennis", "jerry", "tyler", "aaron", "jose", "adam", "nathan",
        "henry", "douglas", "zachary", "peter", "kyle", "noah", "ethan", "jeremy",
        "walter", "christian", "keith", "roger", "terry", "austin", "sean", "gerald",
        "carl", "harold", "dylan", "arthur", "lawrence", "jordan", "jesse", "bryan",
        "billy", "bruce", "gabriel", "joe", "logan", "albert", "willie", "alan", "eugene",
        "ralph", "roy", "louis", "russell", "philip", "harry", "vincent", "bobby", "johnny", "bob",
        "martin", "oliver", "charlie", "lucas", "mason", "liam", "aiden", "jackson",
        "alfie", "archie", "freddie", "oscar", "leo", "teddy", "evan", "blake",
        // ---- US/UK, female ----
        "mary", "patricia", "jennifer", "linda", "barbara", "elizabeth", "susan",
        "jessica", "sarah", "karen", "lisa", "nancy", "betty", "margaret", "sandra",
        "ashley", "kimberly", "emily", "donna", "michelle", "dorothy", "carol", "amanda",
        "melissa", "deborah", "stephanie", "rebecca", "sharon", "laura", "cynthia",
        "kathleen", "amy", "angela", "shirley", "anna", "brenda", "pamela", "emma",
        "nicole", "helen", "samantha", "katherine", "christine", "debra", "rachel",
        "carolyn", "janet", "catherine", "maria", "heather", "diane", "ruth", "julie",
        "olivia", "joyce", "virginia", "victoria", "kelly", "lauren", "christina", "joan",
        "evelyn", "judith", "megan", "andrea", "cheryl", "hannah", "jacqueline", "martha",
        "gloria", "teresa", "ann", "sara", "madison", "frances", "kathryn", "janice",
        "jean", "abigail", "alice", "judy", "sophia", "grace", "denise", "amber", "doris",
        "marilyn", "danielle", "beverly", "isabella", "theresa", "diana", "natalie",
        "brittany", "charlotte", "marie", "kayla", "alexis", "lori", "jane", "claire",
        "julia", "lucy", "ella", "chloe", "mia", "ava", "lily", "zoe", "molly", "ruby",
        "poppy", "isla", "daisy", "freya", "florence", "elsie",
        // ---- Surnames used as first names ----
        "taylor", "morgan", "cameron", "bailey", "parker", "hunter", "carter", "riley",
        // ---- France, Italy, Spain ----
        "pierre", "michel", "sophie", "camille", "lea", "léa", "andré", "rené", "françois",
        "marco", "luca", "matteo", "giulia", "francesca", "elena", "giuseppe",
        "pablo", "carlos", "juan", "antonio", "carmen", "lucia", "josé",
        // ---- Germany, male ----
        "achim", "adrian", "albrecht", "alfred", "andreas", "anton", "armin",
        "arne", "arno", "axel", "benedikt", "bernd", "bernhard", "björn", "boris", "bruno",
        "burkhard", "carsten", "christoph", "claus", "clemens", "constantin", "detlef",
        "dieter", "dietmar", "dietrich", "dirk", "dominik", "eberhard", "eckhard", "egon",
        "elias", "emil", "erich", "erik", "erwin", "fabian", "felix", "finn", "florian",
        "frieder", "friedrich", "fritz", "georg", "gerd", "gerhard", "gernot", "gregor",
        "günter", "günther", "gunter", "gustav", "hannes", "hans", "harald", "hartmut",
        "heiko", "heinrich", "heinz", "helmut", "henning", "herbert", "hermann", "holger",
        "horst", "hubert", "ingo", "jakob", "jannik", "jens", "joachim", "jochen", "johann",
        "johannes", "jonas", "jörg", "joerg", "josef", "jürgen", "juergen", "julian", "kai",
        "karl", "karsten", "klaus", "knut", "konrad", "konstantin", "kurt", "lars", "lennart",
        "leon", "leonhard", "linus", "lorenz", "lothar", "lukas", "ludwig", "malte",
        "manfred", "marcel", "marcus", "mario", "marius", "markus", "mathias", "matthias",
        "max", "maximilian", "moritz", "niklas", "nico", "nils", "norbert", "olaf", "ole",
        "oskar", "otto", "pascal", "philipp", "rainer", "ralf", "ralph", "reinhard",
        "reinhold", "robin", "roland", "rolf", "rüdiger", "ruediger", "rudolf", "sebastian",
        "siegfried", "simon", "sönke", "sören", "stefan", "steffen", "stephan", "sven",
        "thilo", "thorsten", "tim", "timo", "tobias", "torben", "torsten", "ulf",
        "ulrich", "uwe", "valentin", "volker", "werner", "wilfried", "wilhelm", "willi",
        "winfried", "wolfgang", "wolfram",
        // ---- Germany, female ----
        "alexandra", "angelika", "anja", "anke", "anne", "annette", "antje", "astrid",
        "bärbel", "beate", "bettina", "bianca", "birgit", "brigitte", "carina", "carolin",
        "caroline", "christa", "christel", "christiane", "claudia", "cornelia", "dagmar",
        "daniela", "dorothea", "edith", "elfriede", "elisabeth", "elke", "emilia", "erika",
        "eva", "franziska", "frauke", "frieda", "gabriele", "gerda", "gertrud", "gisela",
        "greta", "gudrun", "hannelore", "heidi", "heike", "helga", "helene", "hildegard",
        "ilse", "ines", "inge", "ingrid", "irmgard", "isabel", "jana", "jasmin", "johanna",
        "josephine", "jutta", "karin", "katharina", "kathrin", "katja", "kerstin", "klara",
        "lara", "lena", "lina", "luisa", "magdalena", "maja", "manuela", "mareike",
        "margarete", "marina", "marion", "marlene", "martina", "melanie", "michaela",
        "miriam", "monika", "nadine", "natascha", "nina", "paula", "petra", "regina",
        "renate", "rosemarie", "sabine", "silke", "simone", "sofia", "sonja", "stefanie",
        "susanne", "svenja", "swantje", "tanja", "ulrike", "ursula", "ute", "vanessa",
        "vera", "verena", "veronika", "waltraud", "wiebke", "yvonne",
        // ---- Austria ----
        "alois", "engelbert", "ferdinand", "gottfried", "herwig", "leopold", "sepp",
        "wendelin", "hubertus", "notburga", "rosalia", "theresia", "zita",
        // ---- Switzerland ----
        "urs", "reto", "ueli", "jürg", "juerg", "pirmin", "silvan", "cédric", "dominic",
        "fabio", "gian", "luzius", "regula", "ursina", "seraina", "mirjam", "rahel",
        "corinne", "nadja", "annemarie", "brigitta", "vreni",
    };
    return names;
}

} // namespace anofox
} // namespace duckdb
