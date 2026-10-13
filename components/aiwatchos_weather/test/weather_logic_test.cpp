// Host tests for the weather data path: the staleness rule
// (components/aiwatchos_os/src/weather_data.cpp) plus the National
// Weather Service parsers (src/weather_parse.cpp) driven by REAL
// recorded api.weather.gov responses — captured live 2026-10-13 from
// the Mac for Huber Heights, Ohio (39.864,-84.132):
//   test/nws_points_fixture.json      /points -> gridpoints ILN/46,73
//   test/nws_stations_fixture.json    observationStations?limit=1
//   test/nws_observation_fixture.json KFFO latest observation:
//                                     24.7 C, 56.95% RH, "Partly Cloudy"
// The fixtures are embedded verbatim below (and as the mutated variants
// the edge-case tests need) so the test is hermetic.
//
// No ESP-IDF or hardware needed — plain g++, no cJSON anymore (the NWS
// parser is field-wise scanning by design):
//
//   g++ -std=c++17 -fno-exceptions -fno-rtti \
//       -I components/aiwatchos_os/include \
//       -I components/aiwatchos_weather/include \
//       components/aiwatchos_os/src/weather_data.cpp \
//       components/aiwatchos_weather/src/weather_parse.cpp \
//       components/aiwatchos_weather/test/weather_logic_test.cpp \
//       -o /tmp/weather_logic_test && /tmp/weather_logic_test
#include "aiwatchos/weather_app.hpp"              // weather_is_stale
#include "aiwatchos_weather/weather_service.hpp"  // NWS parsers + conversion

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Verbatim fixtures (see header comment).
static const char kPoints[] =
    "{\n    \"@context\": [\n        \"https://geojson.org/geojson-ld/geojson-context.jsonld\",\n        {\n            \"@version\": \""
    "1.1\",\n            \"wx\": \"https://api.weather.gov/ontology#\",\n            \"s\": \"https://schema.org/\",\n            \"geo\": "
    "\"http://www.opengis.net/ont/geosparql#\",\n            \"unit\": \"http://codes.wmo.int/common/unit/\",\n            \"@vocab\": "
    "\"https://api.weather.gov/ontology#\",\n            \"geometry\": {\n                \"@id\": \"s:GeoCoordinates\",\n              "
    "  \"@type\": \"geo:wktLiteral\"\n            },\n            \"city\": \"s:addressLocality\",\n            \"state\": \"s:addressRegio"
    "n\",\n            \"distance\": {\n                \"@id\": \"s:Distance\",\n                \"@type\": \"s:QuantitativeValue\"\n      "
    "      },\n            \"bearing\": {\n                \"@type\": \"s:QuantitativeValue\"\n            },\n            \"value\": {\n "
    "               \"@id\": \"s:value\"\n            },\n            \"unitCode\": {\n                \"@id\": \"s:unitCode\",\n          "
    "      \"@type\": \"@id\"\n            },\n            \"forecastOffice\": {\n                \"@type\": \"@id\"\n            },\n      "
    "      \"forecastGridData\": {\n                \"@type\": \"@id\"\n            },\n            \"publicZone\": {\n                \"@"
    "type\": \"@id\"\n            },\n            \"county\": {\n                \"@type\": \"@id\"\n            }\n        }\n    ],\n    \"i"
    "d\": \"https://api.weather.gov/points/39.864,-84.132\",\n    \"type\": \"Feature\",\n    \"geometry\": {\n        \"type\": \"Point\",\n "
    "       \"coordinates\": [\n            -84.132,\n            39.864\n        ]\n    },\n    \"properties\": {\n        \"@id\": \"htt"
    "ps://api.weather.gov/points/39.864,-84.132\",\n        \"@type\": \"wx:Point\",\n        \"cwa\": \"ILN\",\n        \"type\": \"land\",\n"
    "        \"forecastOffice\": \"https://api.weather.gov/offices/ILN\",\n        \"gridId\": \"ILN\",\n        \"gridX\": 46,\n        \""
    "gridY\": 73,\n        \"forecast\": \"https://api.weather.gov/gridpoints/ILN/46,73/forecast\",\n        \"forecastHourly\": \"http"
    "s://api.weather.gov/gridpoints/ILN/46,73/forecast/hourly\",\n        \"forecastGridData\": \"https://api.weather.gov/gridpoin"
    "ts/ILN/46,73\",\n        \"observationStations\": \"https://api.weather.gov/gridpoints/ILN/46,73/stations\",\n        \"relative"
    "Location\": {\n            \"type\": \"Feature\",\n            \"geometry\": {\n                \"type\": \"Point\",\n                \""
    "coordinates\": [\n                    -84.1128933,\n                    39.8588807\n                ]\n            },\n       "
    "     \"properties\": {\n                \"city\": \"Huber Heights\",\n                \"state\": \"OH\",\n                \"distance\":"
    " {\n                    \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 0\n                },\n                \"beari"
    "ng\": {\n                    \"unitCode\": \"wmoUnit:degree_(angle)\",\n                    \"value\": 0\n                }\n      "
    "      }\n        },\n        \"forecastZone\": \"https://api.weather.gov/zones/forecast/OHZ061\",\n        \"county\": \"https://a"
    "pi.weather.gov/zones/county/OHC113\",\n        \"fireWeatherZone\": \"https://api.weather.gov/zones/fire/OHZ061\",\n        \"ti"
    "meZone\": \"America/New_York\",\n        \"radarStation\": \"KILN\",\n        \"astronomicalData\": {\n            \"sunrise\": \"2026-"
    "10-13T07:45:03-04:00\",\n            \"sunset\": \"2026-10-13T19:00:21-04:00\",\n            \"transit\": \"2026-10-13T13:22:42-04"
    ":00\",\n            \"civilTwilightBegin\": \"2026-10-13T07:17:58-04:00\",\n            \"civilTwilightEnd\": \"2026-10-13T19:27:2"
    "5-04:00\",\n            \"nauticalTwilightBegin\": \"2026-10-13T06:46:39-04:00\",\n            \"nauticalTwilightEnd\": \"2026-10-"
    "13T19:58:44-04:00\",\n            \"astronomicalTwilightBegin\": \"2026-10-13T06:15:21-04:00\",\n            \"astronomicalTwili"
    "ghtEnd\": \"2026-10-13T20:30:02-04:00\"\n        },\n        \"nwr\": {\n            \"transmitter\": \"WXJ46\",\n            \"sameCo"
    "de\": \"039113\",\n            \"areaBroadcast\": \"https://api.weather.gov/radio/WXJ46/broadcast\",\n            \"pointBroadcast"
    "\": \"https://api.weather.gov/points/39.864,-84.132/radio\"\n        }\n    }\n}";
static const char kStations[] =
    "{\n    \"@context\": [\n        \"https://geojson.org/geojson-ld/geojson-context.jsonld\",\n        {\n            \"@version\": \""
    "1.1\",\n            \"wx\": \"https://api.weather.gov/ontology#\",\n            \"s\": \"https://schema.org/\",\n            \"geo\": "
    "\"http://www.opengis.net/ont/geosparql#\",\n            \"unit\": \"http://codes.wmo.int/common/unit/\",\n            \"@vocab\": "
    "\"https://api.weather.gov/ontology#\",\n            \"geometry\": {\n                \"@id\": \"s:GeoCoordinates\",\n              "
    "  \"@type\": \"geo:wktLiteral\"\n            },\n            \"city\": \"s:addressLocality\",\n            \"state\": \"s:addressRegio"
    "n\",\n            \"distance\": {\n                \"@id\": \"s:Distance\",\n                \"@type\": \"s:QuantitativeValue\"\n      "
    "      },\n            \"bearing\": {\n                \"@type\": \"s:QuantitativeValue\"\n            },\n            \"value\": {\n "
    "               \"@id\": \"s:value\"\n            },\n            \"unitCode\": {\n                \"@id\": \"s:unitCode\",\n          "
    "      \"@type\": \"@id\"\n            },\n            \"forecastOffice\": {\n                \"@type\": \"@id\"\n            },\n      "
    "      \"forecastGridData\": {\n                \"@type\": \"@id\"\n            },\n            \"publicZone\": {\n                \"@"
    "type\": \"@id\"\n            },\n            \"county\": {\n                \"@type\": \"@id\"\n            },\n            \"observati"
    "onStations\": {\n                \"@container\": \"@list\",\n                \"@type\": \"@id\"\n            }\n        }\n    ],\n    "
    "\"type\": \"FeatureCollection\",\n    \"features\": [\n        {\n            \"id\": \"https://api.weather.gov/stations/KFFO\",\n    "
    "        \"type\": \"Feature\",\n            \"geometry\": {\n                \"type\": \"Point\",\n                \"coordinates\": [\n "
    "                   -84.05,\n                    39.83333\n                ]\n            },\n            \"properties\": {\n   "
    "             \"@id\": \"https://api.weather.gov/stations/KFFO\",\n                \"@type\": \"wx:ObservationStation\",\n         "
    "       \"elevation\": {\n                    \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 250.8504\n               "
    " },\n                \"stationIdentifier\": \"KFFO\",\n                \"name\": \"Dayton / Wright-Patterson Air Force Base\",\n   "
    "             \"timeZone\": \"America/New_York\",\n                \"provider\": \"OTHER-MTR\",\n                \"subProvider\": \"\","
    "\n                \"distance\": {\n                    \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 7624.7119338696"
    "\n                },\n                \"bearing\": {\n                    \"unitCode\": \"wmoUnit:degree_(angle)\",\n             "
    "       \"value\": 106\n                },\n                \"forecast\": \"https://api.weather.gov/zones/forecast/OHZ062\",\n    "
    "            \"county\": \"https://api.weather.gov/zones/county/OHC057\",\n                \"fireWeatherZone\": \"https://api.wea"
    "ther.gov/zones/fire/OHZ062\"\n            }\n        }\n    ],\n    \"observationStations\": [\n        \"https://api.weather.gov"
    "/stations/KFFO\"\n    ],\n    \"pagination\": {\n        \"next\": \"https://api.weather.gov/stations?id%5B0%5D=KAID&id%5B1%5D=KA"
    "OH&id%5B2%5D=KAXV&id%5B3%5D=KBAK&id%5B4%5D=KBJJ&id%5B5%5D=KCAK&id%5B6%5D=KCMH&id%5B7%5D=KCRW&id%5B8%5D=KCVG&id%5B9%5D=KD"
    "AY&id%5B10%5D=KDLZ&id%5B11%5D=KEDJ&id%5B12%5D=KEOP&id%5B13%5D=KEYE&id%5B14%5D=KFDY&id%5B15%5D=KFFO&id%5B16%5D=KFGX&id%5B"
    "17%5D=KFWA&id%5B18%5D=KGDK&id%5B19%5D=KGEZ&id%5B20%5D=KHAO&id%5B21%5D=KHTS&id%5B22%5D=KI23&id%5B23%5D=KI67&id%5B24%5D=KI"
    "68&id%5B25%5D=KI69&id%5B26%5D=KI74&id%5B27%5D=KI95&id%5B28%5D=KILN&id%5B29%5D=KIND&id%5B30%5D=KLCK&id%5B31%5D=KLEX&id%5B"
    "32%5D=KLHQ&id%5B33%5D=KLOU&id%5B34%5D=KLUK&id%5B35%5D=KMFD&id%5B36%5D=KMGY&id%5B37%5D=KMIE&id%5B38%5D=KMNN&id%5B39%5D=KM"
    "RT&id%5B40%5D=KOSU&id%5B41%5D=KOXD&id%5B42%5D=KPHD&id%5B43%5D=KPKB&id%5B44%5D=KRID&id%5B45%5D=KRZT&id%5B46%5D=KSDF&id%5B"
    "47%5D=KSGH&id%5B48%5D=KTZR&id%5B49%5D=KUYF&id%5B50%5D=KVES&id%5B51%5D=KVTA&id%5B52%5D=KZZV&cursor=eyJzIjo1MDB9\"\n    }\n}";
static const char kObs[] =
    "{\n    \"@context\": [\n        \"https://geojson.org/geojson-ld/geojson-context.jsonld\",\n        {\n            \"@version\": \""
    "1.1\",\n            \"wx\": \"https://api.weather.gov/ontology#\",\n            \"s\": \"https://schema.org/\",\n            \"geo\": "
    "\"http://www.opengis.net/ont/geosparql#\",\n            \"unit\": \"http://codes.wmo.int/common/unit/\",\n            \"@vocab\": "
    "\"https://api.weather.gov/ontology#\",\n            \"geometry\": {\n                \"@id\": \"s:GeoCoordinates\",\n              "
    "  \"@type\": \"geo:wktLiteral\"\n            },\n            \"city\": \"s:addressLocality\",\n            \"state\": \"s:addressRegio"
    "n\",\n            \"distance\": {\n                \"@id\": \"s:Distance\",\n                \"@type\": \"s:QuantitativeValue\"\n      "
    "      },\n            \"bearing\": {\n                \"@type\": \"s:QuantitativeValue\"\n            },\n            \"value\": {\n "
    "               \"@id\": \"s:value\"\n            },\n            \"unitCode\": {\n                \"@id\": \"s:unitCode\",\n          "
    "      \"@type\": \"@id\"\n            },\n            \"forecastOffice\": {\n                \"@type\": \"@id\"\n            },\n      "
    "      \"forecastGridData\": {\n                \"@type\": \"@id\"\n            },\n            \"publicZone\": {\n                \"@"
    "type\": \"@id\"\n            },\n            \"county\": {\n                \"@type\": \"@id\"\n            }\n        }\n    ],\n    \"i"
    "d\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n    \"type\": \"Feature\",\n    \"geometry"
    "\": {\n        \"type\": \"Point\",\n        \"coordinates\": [\n            -84.05,\n            39.83\n        ]\n    },\n    \"prope"
    "rties\": {\n        \"@id\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n        \"@type\""
    ": \"wx:ObservationStation\",\n        \"elevation\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 251\n        "
    "},\n        \"station\": \"https://api.weather.gov/stations/KFFO\",\n        \"stationId\": \"KFFO\",\n        \"stationName\": \"Dayt"
    "on / Wright-Patterson Air Force Base\",\n        \"timestamp\": \"2026-10-13T14:55:00+00:00\",\n        \"rawMessage\": \"KFFO 131"
    "455Z 23012KT 10SM SCT110 SCT180 25/16 A2993 RMK AO2A SLP132 WND DATA ESTMD T02470156 51005\",\n        \"textDescription\": "
    "\"Partly Cloudy\",\n        \"icon\": \"https://api.weather.gov/icons/land/day/sct?size=medium\",\n        \"presentWeather\": [],"
    "\n        \"temperature\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 24.7,\n            \"qualityControl"
    "\": \"V\"\n        },\n        \"dewpoint\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 15.6,\n            \""
    "qualityControl\": \"V\"\n        },\n        \"windDirection\": {\n            \"unitCode\": \"wmoUnit:degree_(angle)\",\n           "
    " \"value\": 230,\n            \"qualityControl\": \"V\"\n        },\n        \"windSpeed\": {\n            \"unitCode\": \"wmoUnit:km_h"
    "-1\",\n            \"value\": 22.32,\n            \"qualityControl\": \"V\"\n        },\n        \"windGust\": {\n            \"unitCod"
    "e\": \"wmoUnit:km_h-1\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"barometricPressur"
    "e\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n            \"value\": 101360,\n            \"qualityControl\": \"V\"\n        },\n  "
    "      \"seaLevelPressure\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n            \"value\": 101320,\n            \"qualityContr"
    "ol\": \"V\"\n        },\n        \"visibility\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 16090,\n           "
    " \"qualityControl\": \"C\"\n        },\n        \"maxTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n       "
    "     \"value\": null\n        },\n        \"minTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n           "
    " \"value\": null\n        },\n        \"precipitationLastHour\": {\n            \"unitCode\": \"wmoUnit:mm\",\n            \"value\": "
    "null,\n            \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast3Hours\": {\n            \"unitCode\": \"wmoUnit"
    ":mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast6Hours\": {\n     "
    "       \"unitCode\": \"wmoUnit:mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"relati"
    "veHumidity\": {\n            \"unitCode\": \"wmoUnit:percent\",\n            \"value\": 56.954899593299,\n            \"qualityCont"
    "rol\": \"V\"\n        },\n        \"windChill\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null,\n         "
    "   \"qualityControl\": \"V\"\n        },\n        \"heatIndex\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": "
    "24.712711267158333,\n            \"qualityControl\": \"V\"\n        },\n        \"cloudLayers\": [\n            {\n                "
    "\"base\": {\n                    \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 3350\n                },\n            "
    "    \"amount\": \"SCT\"\n            },\n            {\n                \"base\": {\n                    \"unitCode\": \"wmoUnit:m\",\n"
    "                    \"value\": 5490\n                },\n                \"amount\": \"SCT\"\n            }\n        ]\n    }\n}";
// Mutations of kObs, generated from the same verbatim response:
static const char kObsNullTemp[] =   // temperature.value -> null
    "{\n    \"@context\": [\n        \"https://geojson.org/geojson-ld/geojson-context.jsonld\",\n        {\n            \"@version\": \""
    "1.1\",\n            \"wx\": \"https://api.weather.gov/ontology#\",\n            \"s\": \"https://schema.org/\",\n            \"geo\": "
    "\"http://www.opengis.net/ont/geosparql#\",\n            \"unit\": \"http://codes.wmo.int/common/unit/\",\n            \"@vocab\": "
    "\"https://api.weather.gov/ontology#\",\n            \"geometry\": {\n                \"@id\": \"s:GeoCoordinates\",\n              "
    "  \"@type\": \"geo:wktLiteral\"\n            },\n            \"city\": \"s:addressLocality\",\n            \"state\": \"s:addressRegio"
    "n\",\n            \"distance\": {\n                \"@id\": \"s:Distance\",\n                \"@type\": \"s:QuantitativeValue\"\n      "
    "      },\n            \"bearing\": {\n                \"@type\": \"s:QuantitativeValue\"\n            },\n            \"value\": {\n "
    "               \"@id\": \"s:value\"\n            },\n            \"unitCode\": {\n                \"@id\": \"s:unitCode\",\n          "
    "      \"@type\": \"@id\"\n            },\n            \"forecastOffice\": {\n                \"@type\": \"@id\"\n            },\n      "
    "      \"forecastGridData\": {\n                \"@type\": \"@id\"\n            },\n            \"publicZone\": {\n                \"@"
    "type\": \"@id\"\n            },\n            \"county\": {\n                \"@type\": \"@id\"\n            }\n        }\n    ],\n    \"i"
    "d\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n    \"type\": \"Feature\",\n    \"geometry"
    "\": {\n        \"type\": \"Point\",\n        \"coordinates\": [\n            -84.05,\n            39.83\n        ]\n    },\n    \"prope"
    "rties\": {\n        \"@id\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n        \"@type\""
    ": \"wx:ObservationStation\",\n        \"elevation\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 251\n        "
    "},\n        \"station\": \"https://api.weather.gov/stations/KFFO\",\n        \"stationId\": \"KFFO\",\n        \"stationName\": \"Dayt"
    "on / Wright-Patterson Air Force Base\",\n        \"timestamp\": \"2026-10-13T14:55:00+00:00\",\n        \"rawMessage\": \"KFFO 131"
    "455Z 23012KT 10SM SCT110 SCT180 25/16 A2993 RMK AO2A SLP132 WND DATA ESTMD T02470156 51005\",\n        \"textDescription\": "
    "\"Partly Cloudy\",\n        \"icon\": \"https://api.weather.gov/icons/land/day/sct?size=medium\",\n        \"presentWeather\": [],"
    "\n        \"temperature\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null,\n            \"qualityControl"
    "\": \"V\"\n        },\n        \"dewpoint\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 15.6,\n            \""
    "qualityControl\": \"V\"\n        },\n        \"windDirection\": {\n            \"unitCode\": \"wmoUnit:degree_(angle)\",\n           "
    " \"value\": 230,\n            \"qualityControl\": \"V\"\n        },\n        \"windSpeed\": {\n            \"unitCode\": \"wmoUnit:km_h"
    "-1\",\n            \"value\": 22.32,\n            \"qualityControl\": \"V\"\n        },\n        \"windGust\": {\n            \"unitCod"
    "e\": \"wmoUnit:km_h-1\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"barometricPressur"
    "e\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n            \"value\": 101360,\n            \"qualityControl\": \"V\"\n        },\n  "
    "      \"seaLevelPressure\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n            \"value\": 101320,\n            \"qualityContr"
    "ol\": \"V\"\n        },\n        \"visibility\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 16090,\n           "
    " \"qualityControl\": \"C\"\n        },\n        \"maxTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n       "
    "     \"value\": null\n        },\n        \"minTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n           "
    " \"value\": null\n        },\n        \"precipitationLastHour\": {\n            \"unitCode\": \"wmoUnit:mm\",\n            \"value\": "
    "null,\n            \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast3Hours\": {\n            \"unitCode\": \"wmoUnit"
    ":mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast6Hours\": {\n     "
    "       \"unitCode\": \"wmoUnit:mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"relati"
    "veHumidity\": {\n            \"unitCode\": \"wmoUnit:percent\",\n            \"value\": 56.954899593299,\n            \"qualityCont"
    "rol\": \"V\"\n        },\n        \"windChill\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null,\n         "
    "   \"qualityControl\": \"V\"\n        },\n        \"heatIndex\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": "
    "24.712711267158333,\n            \"qualityControl\": \"V\"\n        },\n        \"cloudLayers\": [\n            {\n                "
    "\"base\": {\n                    \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 3350\n                },\n            "
    "    \"amount\": \"SCT\"\n            },\n            {\n                \"base\": {\n                    \"unitCode\": \"wmoUnit:m\",\n"
    "                    \"value\": 5490\n                },\n                \"amount\": \"SCT\"\n            }\n        ]\n    }\n}";
static const char kObsNullHum[] =    // relativeHumidity.value -> null
    "{\n    \"@context\": [\n        \"https://geojson.org/geojson-ld/geojson-context.jsonld\",\n        {\n            \"@version\": \""
    "1.1\",\n            \"wx\": \"https://api.weather.gov/ontology#\",\n            \"s\": \"https://schema.org/\",\n            \"geo\": "
    "\"http://www.opengis.net/ont/geosparql#\",\n            \"unit\": \"http://codes.wmo.int/common/unit/\",\n            \"@vocab\": "
    "\"https://api.weather.gov/ontology#\",\n            \"geometry\": {\n                \"@id\": \"s:GeoCoordinates\",\n              "
    "  \"@type\": \"geo:wktLiteral\"\n            },\n            \"city\": \"s:addressLocality\",\n            \"state\": \"s:addressRegio"
    "n\",\n            \"distance\": {\n                \"@id\": \"s:Distance\",\n                \"@type\": \"s:QuantitativeValue\"\n      "
    "      },\n            \"bearing\": {\n                \"@type\": \"s:QuantitativeValue\"\n            },\n            \"value\": {\n "
    "               \"@id\": \"s:value\"\n            },\n            \"unitCode\": {\n                \"@id\": \"s:unitCode\",\n          "
    "      \"@type\": \"@id\"\n            },\n            \"forecastOffice\": {\n                \"@type\": \"@id\"\n            },\n      "
    "      \"forecastGridData\": {\n                \"@type\": \"@id\"\n            },\n            \"publicZone\": {\n                \"@"
    "type\": \"@id\"\n            },\n            \"county\": {\n                \"@type\": \"@id\"\n            }\n        }\n    ],\n    \"i"
    "d\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n    \"type\": \"Feature\",\n    \"geometry"
    "\": {\n        \"type\": \"Point\",\n        \"coordinates\": [\n            -84.05,\n            39.83\n        ]\n    },\n    \"prope"
    "rties\": {\n        \"@id\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n        \"@type\""
    ": \"wx:ObservationStation\",\n        \"elevation\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 251\n        "
    "},\n        \"station\": \"https://api.weather.gov/stations/KFFO\",\n        \"stationId\": \"KFFO\",\n        \"stationName\": \"Dayt"
    "on / Wright-Patterson Air Force Base\",\n        \"timestamp\": \"2026-10-13T14:55:00+00:00\",\n        \"rawMessage\": \"KFFO 131"
    "455Z 23012KT 10SM SCT110 SCT180 25/16 A2993 RMK AO2A SLP132 WND DATA ESTMD T02470156 51005\",\n        \"textDescription\": "
    "\"Partly Cloudy\",\n        \"icon\": \"https://api.weather.gov/icons/land/day/sct?size=medium\",\n        \"presentWeather\": [],"
    "\n        \"temperature\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 24.7,\n            \"qualityControl"
    "\": \"V\"\n        },\n        \"dewpoint\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 15.6,\n            \""
    "qualityControl\": \"V\"\n        },\n        \"windDirection\": {\n            \"unitCode\": \"wmoUnit:degree_(angle)\",\n           "
    " \"value\": 230,\n            \"qualityControl\": \"V\"\n        },\n        \"windSpeed\": {\n            \"unitCode\": \"wmoUnit:km_h"
    "-1\",\n            \"value\": 22.32,\n            \"qualityControl\": \"V\"\n        },\n        \"windGust\": {\n            \"unitCod"
    "e\": \"wmoUnit:km_h-1\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"barometricPressur"
    "e\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n            \"value\": 101360,\n            \"qualityControl\": \"V\"\n        },\n  "
    "      \"seaLevelPressure\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n            \"value\": 101320,\n            \"qualityContr"
    "ol\": \"V\"\n        },\n        \"visibility\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 16090,\n           "
    " \"qualityControl\": \"C\"\n        },\n        \"maxTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n       "
    "     \"value\": null\n        },\n        \"minTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n           "
    " \"value\": null\n        },\n        \"precipitationLastHour\": {\n            \"unitCode\": \"wmoUnit:mm\",\n            \"value\": "
    "null,\n            \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast3Hours\": {\n            \"unitCode\": \"wmoUnit"
    ":mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast6Hours\": {\n     "
    "       \"unitCode\": \"wmoUnit:mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"relati"
    "veHumidity\": {\n            \"unitCode\": \"wmoUnit:percent\",\n            \"value\": null,\n            \"qualityControl\": \"V\"\n "
    "       },\n        \"windChill\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null,\n            \"quality"
    "Control\": \"V\"\n        },\n        \"heatIndex\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 24.71271126"
    "7158333,\n            \"qualityControl\": \"V\"\n        },\n        \"cloudLayers\": [\n            {\n                \"base\": {\n "
    "                   \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 3350\n                },\n                \"amount"
    "\": \"SCT\"\n            },\n            {\n                \"base\": {\n                    \"unitCode\": \"wmoUnit:m\",\n           "
    "         \"value\": 5490\n                },\n                \"amount\": \"SCT\"\n            }\n        ]\n    }\n}";
static const char kObsNoDesc[] =     // textDescription removed entirely
    "{\n    \"@context\": [\n        \"https://geojson.org/geojson-ld/geojson-context.jsonld\",\n        {\n            \"@version\": \""
    "1.1\",\n            \"wx\": \"https://api.weather.gov/ontology#\",\n            \"s\": \"https://schema.org/\",\n            \"geo\": "
    "\"http://www.opengis.net/ont/geosparql#\",\n            \"unit\": \"http://codes.wmo.int/common/unit/\",\n            \"@vocab\": "
    "\"https://api.weather.gov/ontology#\",\n            \"geometry\": {\n                \"@id\": \"s:GeoCoordinates\",\n              "
    "  \"@type\": \"geo:wktLiteral\"\n            },\n            \"city\": \"s:addressLocality\",\n            \"state\": \"s:addressRegio"
    "n\",\n            \"distance\": {\n                \"@id\": \"s:Distance\",\n                \"@type\": \"s:QuantitativeValue\"\n      "
    "      },\n            \"bearing\": {\n                \"@type\": \"s:QuantitativeValue\"\n            },\n            \"value\": {\n "
    "               \"@id\": \"s:value\"\n            },\n            \"unitCode\": {\n                \"@id\": \"s:unitCode\",\n          "
    "      \"@type\": \"@id\"\n            },\n            \"forecastOffice\": {\n                \"@type\": \"@id\"\n            },\n      "
    "      \"forecastGridData\": {\n                \"@type\": \"@id\"\n            },\n            \"publicZone\": {\n                \"@"
    "type\": \"@id\"\n            },\n            \"county\": {\n                \"@type\": \"@id\"\n            }\n        }\n    ],\n    \"i"
    "d\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n    \"type\": \"Feature\",\n    \"geometry"
    "\": {\n        \"type\": \"Point\",\n        \"coordinates\": [\n            -84.05,\n            39.83\n        ]\n    },\n    \"prope"
    "rties\": {\n        \"@id\": \"https://api.weather.gov/stations/KFFO/observations/2026-10-13T14:55:00+00:00\",\n        \"@type\""
    ": \"wx:ObservationStation\",\n        \"elevation\": {\n            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 251\n        "
    "},\n        \"station\": \"https://api.weather.gov/stations/KFFO\",\n        \"stationId\": \"KFFO\",\n        \"stationName\": \"Dayt"
    "on / Wright-Patterson Air Force Base\",\n        \"timestamp\": \"2026-10-13T14:55:00+00:00\",\n        \"rawMessage\": \"KFFO 131"
    "455Z 23012KT 10SM SCT110 SCT180 25/16 A2993 RMK AO2A SLP132 WND DATA ESTMD T02470156 51005\",\n        \"icon\": \"https://ap"
    "i.weather.gov/icons/land/day/sct?size=medium\",\n        \"presentWeather\": [],\n        \"temperature\": {\n            \"unitC"
    "ode\": \"wmoUnit:degC\",\n            \"value\": 24.7,\n            \"qualityControl\": \"V\"\n        },\n        \"dewpoint\": {\n    "
    "        \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 15.6,\n            \"qualityControl\": \"V\"\n        },\n        \"win"
    "dDirection\": {\n            \"unitCode\": \"wmoUnit:degree_(angle)\",\n            \"value\": 230,\n            \"qualityControl\":"
    " \"V\"\n        },\n        \"windSpeed\": {\n            \"unitCode\": \"wmoUnit:km_h-1\",\n            \"value\": 22.32,\n           "
    " \"qualityControl\": \"V\"\n        },\n        \"windGust\": {\n            \"unitCode\": \"wmoUnit:km_h-1\",\n            \"value\": n"
    "ull,\n            \"qualityControl\": \"Z\"\n        },\n        \"barometricPressure\": {\n            \"unitCode\": \"wmoUnit:Pa\",\n"
    "            \"value\": 101360,\n            \"qualityControl\": \"V\"\n        },\n        \"seaLevelPressure\": {\n            \"uni"
    "tCode\": \"wmoUnit:Pa\",\n            \"value\": 101320,\n            \"qualityControl\": \"V\"\n        },\n        \"visibility\": {\n"
    "            \"unitCode\": \"wmoUnit:m\",\n            \"value\": 16090,\n            \"qualityControl\": \"C\"\n        },\n        \"m"
    "axTemperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null\n        },\n        \"minTe"
    "mperatureLast24Hours\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null\n        },\n        \"precipita"
    "tionLastHour\": {\n            \"unitCode\": \"wmoUnit:mm\",\n            \"value\": null,\n            \"qualityControl\": \"Z\"\n    "
    "    },\n        \"precipitationLast3Hours\": {\n            \"unitCode\": \"wmoUnit:mm\",\n            \"value\": null,\n           "
    " \"qualityControl\": \"Z\"\n        },\n        \"precipitationLast6Hours\": {\n            \"unitCode\": \"wmoUnit:mm\",\n           "
    " \"value\": null,\n            \"qualityControl\": \"Z\"\n        },\n        \"relativeHumidity\": {\n            \"unitCode\": \"wmoU"
    "nit:percent\",\n            \"value\": 56.954899593299,\n            \"qualityControl\": \"V\"\n        },\n        \"windChill\": {\n"
    "            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": null,\n            \"qualityControl\": \"V\"\n        },\n        "
    "\"heatIndex\": {\n            \"unitCode\": \"wmoUnit:degC\",\n            \"value\": 24.712711267158333,\n            \"qualityCont"
    "rol\": \"V\"\n        },\n        \"cloudLayers\": [\n            {\n                \"base\": {\n                    \"unitCode\": \"w"
    "moUnit:m\",\n                    \"value\": 3350\n                },\n                \"amount\": \"SCT\"\n            },\n         "
    "   {\n                \"base\": {\n                    \"unitCode\": \"wmoUnit:m\",\n                    \"value\": 5490\n          "
    "      },\n                \"amount\": \"SCT\"\n            }\n        ]\n    }\n}";

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    using namespace aiwatchos_weather;

    // --- Celsius -> Fahrenheit conversion vectors -------------------------
    CHECK(weather_fahrenheit_from_celsius(-40.0) == -40);
    CHECK(weather_fahrenheit_from_celsius(0.0) == 32);
    CHECK(weather_fahrenheit_from_celsius(100.0) == 212);
    CHECK(weather_fahrenheit_from_celsius(37.0) == 99);    // 98.6 rounds up
    CHECK(weather_fahrenheit_from_celsius(21.1) == 70);    // 69.98 -> 70
    CHECK(weather_fahrenheit_from_celsius(-17.8) == 0);    // -0.04 -> 0
    CHECK(weather_fahrenheit_from_celsius(24.7) == 76);    // the fixture

    // --- Chain walk over the real responses -------------------------------
    char url[256];
    CHECK(weather_nws_parse_stations_url(kPoints, url, sizeof(url)));
    CHECK(std::strcmp(url,
                      "https://api.weather.gov/gridpoints/ILN/46,73/"
                      "stations") == 0);

    char station[16];
    CHECK(weather_nws_parse_station_id(kStations, station, sizeof(station)));
    CHECK(std::strcmp(station, "KFFO") == 0);

    float temp_f = 0;
    int humidity = -1;
    bool has_humidity = false;
    char desc[48];
    CHECK(weather_nws_parse_observation(kObs, &temp_f, &humidity,
                                        &has_humidity, desc, sizeof(desc)));
    CHECK(near(temp_f, 76.0f));          // 24.7 C -> 76 F
    CHECK(has_humidity);
    CHECK(humidity == 57);               // 56.9548... rounds to 57
    CHECK(std::strcmp(desc, "Partly Cloudy") == 0);

    // --- Null temperature: the station has not reported — hard failure ---
    CHECK(!weather_nws_parse_observation(kObsNullTemp, &temp_f, &humidity,
                                         &has_humidity, desc, sizeof(desc)));

    // --- Null humidity: valid reading, just no humidity to show -----------
    humidity = -1;
    has_humidity = true;               // must be reset by the parser
    desc[0] = '\0';
    CHECK(weather_nws_parse_observation(kObsNullHum, &temp_f, &humidity,
                                        &has_humidity, desc, sizeof(desc)));
    CHECK(near(temp_f, 76.0f));
    CHECK(!has_humidity);
    CHECK(std::strcmp(desc, "Partly Cloudy") == 0);

    // --- Missing textDescription: valid, description stays empty ----------
    CHECK(weather_nws_parse_observation(kObsNoDesc, &temp_f, &humidity,
                                        &has_humidity, desc, sizeof(desc)));
    CHECK(near(temp_f, 76.0f));
    CHECK(has_humidity && humidity == 57);
    CHECK(desc[0] == '\0');

    // --- Truncated bodies never parse --------------------------------------
    // The scanner is field-wise: it fails exactly when a mandatory field
    // is cut. Truncating inside the temperature object (the one required
    // datum) must fail; the points/stations strings sit late in their
    // documents, so a half cut removes them entirely.
    std::string trunc_obs = kObs;
    size_t temp_at = trunc_obs.find("\"temperature\"");
    CHECK(temp_at != std::string::npos);
    trunc_obs.resize(temp_at + 20);   // inside the temperature object
    CHECK(!weather_nws_parse_observation(trunc_obs.c_str(), &temp_f,
                                         &humidity, &has_humidity, desc,
                                         sizeof(desc)));
    std::string trunc_points = kPoints;
    trunc_points.resize(trunc_points.size() / 2);
    CHECK(!weather_nws_parse_stations_url(trunc_points.c_str(), url,
                                          sizeof(url)));
    std::string trunc_stations = kStations;
    trunc_stations.resize(trunc_stations.size() / 2);
    CHECK(!weather_nws_parse_station_id(trunc_stations.c_str(), station,
                                        sizeof(station)));

    // --- Garbage inputs -----------------------------------------------------
    const char* kJunk[] = {"", "{", "{}", "null", "not json"};
    for (const char* j : kJunk) {
        CHECK(!weather_nws_parse_stations_url(j, url, sizeof(url)));
        CHECK(!weather_nws_parse_station_id(j, station, sizeof(station)));
        CHECK(!weather_nws_parse_observation(j, &temp_f, &humidity,
                                             &has_humidity, desc,
                                             sizeof(desc)));
    }
    CHECK(!weather_nws_parse_stations_url(nullptr, url, sizeof(url)));
    CHECK(!weather_nws_parse_station_id(nullptr, station, sizeof(station)));
    CHECK(!weather_nws_parse_observation(nullptr, &temp_f, &humidity,
                                         &has_humidity, desc, sizeof(desc)));
    CHECK(!weather_nws_parse_observation(kObs, nullptr, &humidity,
                                         &has_humidity, desc, sizeof(desc)));

    // Undersized output buffers fail rather than truncating silently.
    char tiny[8];
    char tiny4[4];   // "KFFO" + NUL needs 5
    CHECK(!weather_nws_parse_stations_url(kPoints, tiny, sizeof(tiny)));
    CHECK(!weather_nws_parse_station_id(kStations, tiny4, sizeof(tiny4)));

    // --- Staleness rule ------------------------------------------------------
    const uint32_t kInterval = 30u * 60000u;   // default refresh: 30 min
    const uint64_t kFetched = 100000000ull;
    CHECK(!aiwatchos::weather_is_stale(kFetched, kFetched, kInterval));
    CHECK(!aiwatchos::weather_is_stale(kFetched + kInterval, kFetched,
                                       kInterval));   // exactly one interval: fresh
    CHECK(aiwatchos::weather_is_stale(kFetched + kInterval + 1, kFetched,
                                      kInterval));    // past it: stale
    CHECK(aiwatchos::weather_is_stale(kFetched + 60ull * 60000ull, kFetched,
                                      kInterval));
    CHECK(aiwatchos::weather_is_stale(kFetched, 0, kInterval));  // never fetched
    CHECK(!aiwatchos::weather_is_stale(kFetched - 5000, kFetched,
                                       kInterval));   // clock skew: not stale

    if (g_failures == 0) std::printf("weather_logic_test: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
