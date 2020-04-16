DROP DATABASE IF EXISTS gateway;

CREATE DATABASE gateway;

\c gateway

ALTER DATABASE gateway OWNER TO pi;

CREATE TABLE esp32 (
    utc numeric(10,0),
    timedate character varying(100),
    dht22_t_esp real,
    dht22_h_esp real,
    sht85_t_esp real,
    sht85_h_esp real,
    hih8121_t_esp real,
    hih8121_h_esp real,
    tmp36_0_esp real,
    tmp36_1_esp real,
    tmp36_2_esp real,
    hih4030_esp real,
    hh10d_esp real,
    dht22_t_wis real,
    dht22_h_wis real,
    sht85_t_wis real,
    sht85_h_wis real,
    hih8121_t_wis real,
    hih8121_h_wis real,
    tmp102_wis real,
    hh10d_wis real,
    dht22_t_mkr real,
    dht22_h_mkr real,
    sht85_t_mkr real,
    sht85_h_mkr real,
    hih8121_t_mkr real,
    hih8121_h_mkr real,
    hh10d_mkr real
);


ALTER TABLE esp32 OWNER TO pi;

--
-- Name: pend_msgs; Type: TABLE; Schema: public; Owner: root
--

CREATE TABLE pend_msgs (
    dev_id numeric(3,0) NOT NULL,
    msg bytea
);


ALTER TABLE pend_msgs OWNER TO pi;



