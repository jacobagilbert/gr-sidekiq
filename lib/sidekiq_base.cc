/*
* Copyright 2018    US Naval Research Lab
* Copyright 2018    Epiq Solutions
*
*
* This is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 3, or (at your option)
* any later version.
*
* This software is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this software; see the file COPYING. If not, write to
* the Free Software Foundation, Inc., 51 Franklin Street,
* Boston, MA 02110-1301, USA.
*/

#include <stdexcept>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include <sidekiq/sidekiq_base.h>
#include <chrono>

using namespace gr::sidekiq;
using std::chrono::duration_cast;
using std::chrono::system_clock;
using std::chrono::nanoseconds;
using std::chrono::microseconds;
using std::chrono::seconds;

static const int SYNC_ZERO{1};

static const int SYNC_GPS_PPS{2};

static const int SYNC_SYSTEM_TIME{3};

static const int NUM_CARDS{1};

static const size_t NANOSECONDS_IN_SECOND{1000000000L};


template<typename HdlType>
sidekiq_base<HdlType>::sidekiq_base(
        int input_card_number,
		int sync_type,
		HdlType port1_handle,
		HdlType port2_handle,
		gr::sidekiq::sidekiq_functions<HdlType> sidekiq_functions) :
		sidekiq_functions(sidekiq_functions) {
	card = input_card_number;
	hdl = port1_handle;
	hdl2 = port2_handle;
    dual_channel = false;
    debug_ctr = 0;

	int32_t status{skiq_init(skiq_xport_type_pcie, skiq_xport_init_level_full, &card, NUM_CARDS)};
	if (status != 0) {
		printf("Error: unable to initialize libsidekiq with status %d\n", status);
        throw std::runtime_error("Failure: skiq_init");
	}
	timestamp_frequency = get_sys_timestamp_frequency();
	sidekiq_system_time_interval_nanos = NANOSECONDS_IN_SECOND / timestamp_frequency;

    // determine radio capabilities
    skiq_read_parameters( card, &sidekiq_params );
    // update scaling parameters based on radio capabilities
    adc_scaling = (pow(2.0f, sidekiq_params.rx_param[hdl].iq_resolution) / 2.0)-1;
    dac_scaling = (pow(2.0f, sidekiq_params.tx_param[hdl].iq_resolution) / 2.0)-1;
        
	set_sync_type(sync_type);
}

template<typename HdlType>
pmt::pmt_t sidekiq_base<HdlType>::get_pmt_tuple_from_timestamp(size_t timestamp) {
	uint64_t seconds{timestamp / NANOSECONDS_IN_SECOND};
	double fractional{static_cast<double>(timestamp % NANOSECONDS_IN_SECOND) / 1e9};
	return pmt::make_tuple(pmt::from_uint64(seconds), pmt::from_double(fractional));
}

template<typename HdlType>
pmt::pmt_t sidekiq_base<HdlType>::get_pmt_cons_from_timestamp(size_t timestamp) {
	uint64_t seconds{timestamp / NANOSECONDS_IN_SECOND};
	double fractional{static_cast<double>(timestamp % NANOSECONDS_IN_SECOND) / 1e9};
	return pmt::cons(pmt::from_long(seconds), pmt::from_double(fractional));
}

template<typename HdlType>
pmt::pmt_t sidekiq_base<HdlType>::get_telemetry_pmt() {
	auto sidekiq_time_nanos = get_sidekiq_system_timestamp() * sidekiq_system_time_interval_nanos;
	auto system_clock_nanos = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
	pmt::pmt_t result = pmt::make_dict();

	result = pmt::dict_add(result, CMD_CURRENT_USRP_TIME, get_pmt_cons_from_timestamp(sidekiq_time_nanos));
	result = pmt::dict_add(result, CMD_CURRENT_HOST_TIME, get_pmt_cons_from_timestamp(system_clock_nanos));
	return result;
}

template<typename HdlType>
void sidekiq_base<HdlType>::set_sync_type(int type) {
	if (type == SYNC_ZERO) {
		set_zero_timestamp();
	} else if (type == SYNC_GPS_PPS) {
		//Note: This is done as an attempt to keep consistency with our GR-usrp block, however it is not directly
		//applicable since the sidekiq does not have an internal GPS device.
		//In order to make this happen we need to either provide a /dev/<gpsSerialPort>, GPSD server, etc....
		//Currently the GRC parameter is commented out for this....

		//get last gps time (full seconds)
		//add one second
		//call set_next_pps_timestamp
	} else if (type == SYNC_SYSTEM_TIME) {
		auto system_clock_nanos = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
		auto sidekiq_timestamp = system_clock_nanos / sidekiq_system_time_interval_nanos;

		set_sidekiq_system_timestamp(sidekiq_timestamp);
	}
}


template<typename HdlType>
void sidekiq_base<HdlType>::get_configuration_limits() {

    printf("\nNumber of RX channels available: %u\n", sidekiq_params.rf_param.num_rx_channels);
    for( int i=0; i<sidekiq_params.rf_param.num_rx_channels; i++ )
    {
        printf("        RX Channel %u\n", i);
	    printf(
			"\tRX LO Range Min/Max: %1.1fMHz,%1.1fMHz\n",
			static_cast<double >(sidekiq_params.rx_param[i].lo_freq_min) / 1e6,
			static_cast<double >(sidekiq_params.rx_param[i].lo_freq_max) / 1e6
	    );
	    printf(
			"\tRX Sample Rate Min/Max: %1.3fMsps,%1.3fMsps\n",
			static_cast<double >(sidekiq_params.rx_param[i].sample_rate_min) / 1e6,
			static_cast<double >(sidekiq_params.rx_param[i].sample_rate_max) / 1e6
	    );
        printf("\tResolution %u\n", sidekiq_params.rx_param[i].iq_resolution);
    }

    printf("Number of TX channels available: %u\n", sidekiq_params.rf_param.num_tx_channels);
    for( int i=0; i<sidekiq_params.rf_param.num_tx_channels; i++ )
    {
        printf("        TX Channel %u\n", i);
        printf(
			"TX LO Range Min/Max: %1.1fMHz,%1.1fMHz\n",
			static_cast<double >(sidekiq_params.tx_param[i].lo_freq_min) / 1e6,
			static_cast<double >(sidekiq_params.tx_param[i].lo_freq_max) / 1e6
	    );
	    printf(
			"TX Sample Rate Min/Max: %1.3fMsps,%1.3fMsps\n",
			static_cast<double >(sidekiq_params.tx_param[i].sample_rate_min) / 1e6,
			static_cast<double >(sidekiq_params.tx_param[i].sample_rate_max) / 1e6
	    );
        printf(
                        "TX Attenuation Min/Max (in quarter dB): %u,%u\n",
                        sidekiq_params.tx_param[i].atten_quarter_db_min,
			sidekiq_params.tx_param[i].atten_quarter_db_max
	    );
        printf("\tResolution %u\n", sidekiq_params.rx_param[i].iq_resolution);
    }
}

template<typename HdlType>
int32_t sidekiq_base<HdlType>::get_ref_clock_configuration() {
	skiq_ref_clock_select_t p_ref_clk;

	if (skiq_read_ref_clock_select(card, &p_ref_clk) != 0) {
		printf("Error: failed to get reference clock configuration\n");
        throw std::runtime_error("Failure: skiq_read_ref_clock_select");
	}
	return p_ref_clk;
}

template<typename HdlType>
void sidekiq_base<HdlType>::set_zero_timestamp() {
	if (skiq_reset_timestamps(card) != 0) {
		printf("Error: failed to set timestamp to zero\n");
        throw std::runtime_error("Failure: skiq_reset_timestamps");
	}
}

template<typename HdlType>
void sidekiq_base<HdlType>::set_next_pps_timestamp() {
	uint64_t future_sys_timestamp{};
	uint64_t new_timestamp{};

	if (skiq_write_timestamp_update_on_1pps(card, future_sys_timestamp, new_timestamp) != 0) {
		printf("Error: failed to set next PPS timestamp\n");
        throw std::runtime_error("Failure: skiq_write_timestamp_update_on_1pps");
	}
}

template<typename HdlType>
uint64_t sidekiq_base<HdlType>::get_last_pps_timestamp() {
	uint64_t rf_timestamp;
	uint64_t system_timestamp;

	if (skiq_read_last_1pps_timestamp(card, &rf_timestamp, &system_timestamp) != 0) {
		printf("Error: failed to get last PPS timestamp\n");
        throw std::runtime_error("Failure: skiq_read_last_1pps_timestamp");
	}
	printf("%ld %ld\n", rf_timestamp, system_timestamp);
	return system_timestamp;
}

template<typename HdlType>
void sidekiq_base<HdlType>::set_sidekiq_system_timestamp(uint64_t timestamp) {
	if (skiq_update_timestamps(card, timestamp) != 0) {
		printf("Error: failed to set system timestamp\n");
        throw std::runtime_error("Failure: skiq_update_timestamps");
	}
}

template<typename HdlType>
uint64_t sidekiq_base<HdlType>::get_sidekiq_system_timestamp() {
	uint64_t timestamp;

	if (skiq_read_curr_sys_timestamp(card, &timestamp) != 0) {
		printf("Error: failed to get system timestamp\n");
        throw std::runtime_error("Failure: skiq_read_curr_sys_timestamp");
	}
	return timestamp;
}

template<typename HdlType>
uint64_t sidekiq_base<HdlType>::get_sys_timestamp_frequency() {
	uint64_t timestamp_frequency;

	if (skiq_read_sys_timestamp_freq(card, &timestamp_frequency) != 0) {
		printf("Error: failed to get system timestamp frequency\n");
        throw std::runtime_error("Failure: skiq_read_sys_timestamp_freq");
	}
	return timestamp_frequency;
}

template<typename HdlType>
void sidekiq_base<HdlType>::read_accelerometer() {
	int32_t status;
	bool supported{false};
	int16_t x_data{};
	int16_t y_data{};
	int16_t z_data{};

	skiq_is_accel_supported(card, &supported);
	if (!supported) {
		printf("Info: accelerometer not supported with product\r\n");
	} else {
		skiq_write_accel_state(card, static_cast<uint8_t>(true));
		status = skiq_read_accel(card, &x_data, &y_data, &z_data);
		skiq_write_accel_state(card, static_cast<uint8_t>(false));
		if (status != 0) {
			printf("Error: Unable to read the accelerometer\r\n");
            throw std::runtime_error("Failure: skiq_write_accel_state");
		}
	}
}

template<typename HdlType>
uint8_t sidekiq_base<HdlType>::read_rfic_register(uint16_t address) {
	uint8_t result;

	if (skiq_read_rfic_reg(card, address, &result) != 0) {
		printf("Error: failed to read RFIC address 0x%04X\n", address);
        throw std::runtime_error("Failure: skiq_read_rfic_reg");
	}
	return result;
}

template<typename HdlType>
void sidekiq_base<HdlType>::write_rfic_register(uint16_t address, uint8_t data) {
	if (skiq_write_rfic_reg(card, address, data) != 0) {
            printf("Error: failed to write RFIC address 0x%08x\n", address);
            throw std::runtime_error("Failure: skiq_write_rfic_reg");
	}
}

template<typename HdlType>
float sidekiq_base<HdlType>::read_temperature() {
	int8_t temp{-1};
	if (skiq_read_temp(card, &temp) != 0) {
		printf("Error: failed to read sidekiq on-board temperature\r\n");
        throw std::runtime_error("Failure: skiq_read_temp");
	}
	return static_cast<float>(temp);    //TODO: there is probably some converation that needs to happen here, check the docs
}


template<typename HdlType>
int sidekiq_base<HdlType>::start_streaming() {
    int status;
    if (dual_channel) {
        status = sidekiq_functions.start_streaming_func(card, hdl2);
        if (status != 0) {
            printf("Error: could not start streaming, status %d, %s\n", status, strerror(abs(status)) );
            throw std::runtime_error("Failure: start streaming");
        }
    }

	status = sidekiq_functions.start_streaming_func(card, hdl);
	if (status != 0) {
		printf("Error: could not start streaming, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: start streaming");
	}
	return status;
}

template<typename HdlType>
int sidekiq_base<HdlType>::stop_streaming() {
    int status;
    if (dual_channel) {
        status = sidekiq_functions.stop_streaming_func(card, hdl2);
        if (status != 0) {
            printf("Error: could not stop streaming, status %d, %s\n", status, strerror(abs(status)) );
            throw std::runtime_error("Failure: stop streaming");
        }
    }

	status = sidekiq_functions.stop_streaming_func(card, hdl);
	if (status != 0) {
		printf("Error: could not stop streaming, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: stop streaming");
	}
	return status;
}

template<typename HdlType>
double sidekiq_base<HdlType>::get_sample_rate(HdlType handle) {
    int status;
	uint32_t p_rate;
	double p_actual_rate;
	uint32_t p_bandwidth;
	uint32_t p_actual_bandwidth;

	status = sidekiq_functions.get_sample_rate_func(card, handle, &p_rate, &p_actual_rate, &p_bandwidth, &p_actual_bandwidth);
	if (status != 0) {
		printf("Error: failed to get sample rate, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: get_samplerate");
	}
	return p_actual_rate;
}

template<typename HdlType>
int sidekiq_base<HdlType>::set_samplerate_bandwidth(double sample_rate, double bandwidth) {
    int status;
	auto rate = static_cast<uint32_t>(sample_rate);
	auto bw = static_cast<uint32_t>(bandwidth);


	status = sidekiq_functions.set_sample_rate_func(card, hdl, rate, bw);
	if (status != 0) {
		printf("Error: could not set sample_rate, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: set samplerate");
        return(status);
	} else {
		this->sample_rate = rate;
		this->bandwidth = bw;
	}
    
    if (dual_channel) {
        status = sidekiq_functions.set_sample_rate_func(card, hdl2, rate, bw);
        if (status != 0) {
            printf("Error: could not set sample_rate, status %d, %s\n", status, strerror(abs(status)) );
            throw std::runtime_error("Failure: set samplerate");
        }
    }
	return status;
}

template<typename HdlType>
double sidekiq_base<HdlType>::get_frequency( HdlType handle) {
    int status;
	uint64_t p_freq;
	double p_actual_freq;

	status = sidekiq_functions.get_frequency_func(card, handle, &p_freq, &p_actual_freq);
	if (status != 0){
		printf("Error: failed to get frequency, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: get frequency");
	}
	return p_actual_freq;
}


template<typename HdlType>
int sidekiq_base<HdlType>::set_frequency(double value) {
    int status;

    if (dual_channel) {
        status = sidekiq_functions.set_frequency_func(card, hdl2, static_cast<uint64_t>(value));
        if (status != 0){
            printf("Error: failed to set frequency, status %d, %s\n", status, strerror(abs(status)) );
            throw std::runtime_error("Failure: set frequency");
        }
    }

	status = sidekiq_functions.set_frequency_func(card, hdl, static_cast<uint64_t>(value));
	if (status != 0){
		printf("Error: failed to set frequency, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: set frequency");
	}
	return status;
}

template<typename HdlType>
uint64_t sidekiq_base<HdlType>::get_timestamp(HdlType handle) {
	uint64_t timestamp;
    int status;

	status = sidekiq_functions.get_timestamp_func(card, handle, &timestamp);
	if (status != 0) {
		printf("Error: failed to get sidekiq system timestamp, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure: get timestamp");
	}
	return timestamp;
}

/**
    @return int32_t  number of nanoseconds elapsed between start and end of set_rx_frequency() call
*/
template<typename HdlType>
int64_t sidekiq_base<HdlType>::get_set_frequency_call_latency(HdlType handle) {
	auto start = std::chrono::system_clock::now();
	uint64_t frequency{static_cast<uint64_t>(2400e6)};

	sidekiq_functions.set_frequency_func(card, handle, frequency);
	return duration_cast<nanoseconds>(system_clock::now() - start).count();
}

template<typename HdlType>
void sidekiq_base<HdlType>::set_filter_parameters(int16_t *coeffs) {
    int status;

//	EPIQ_API int32_t skiq_write_rfic_rx_fir_coeffs(uint8_t card, int16_t *p_coeffs);
//	EPIQ_API int32_t skiq_write_rx_fir_gain(uint8_t card, skiq_rx_hdl_t hdl, skiq_rx_fir_gain_t gain);
	status = sidekiq_functions.set_rfic_fir_coeffs_func(card, coeffs);
	if (status != 0) {
		printf("Error: failed to set fir coeffs, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure:  set fir coeffs");
	}
}

template<typename HdlType>
void sidekiq_base<HdlType>::get_filter_parameters() {
    int status;
	uint8_t num_taps;
	uint8_t decimation;
	int16_t coeffs[128];

	status = sidekiq_functions.get_rfic_fir_config_func(card, &num_taps, &decimation);
	if (status != 0) {
		printf("Error: failed to get fir config, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure:  get fir config");
	}

	status = sidekiq_functions.get_rfic_fir_coeffs_func(card, reinterpret_cast<int16_t *>(&coeffs));
	if (status != 0) {
		printf("Error: failed to get fir coeffs, status %d, %s\n", status, strerror(abs(status)) );
        throw std::runtime_error("Failure:  get fir coeffs");
        exit(status);
	}

	printf("FIR decimation: %d\n", decimation);
	printf("FIR num taps: %d\n", num_taps);
	printf("FIR taps:\n");
	for (int count = 0; count < num_taps; count++) {
		printf("%03d,%05d\n", count, coeffs[count]);
	}
	printf("\n\n");
}


template
class sidekiq_base<skiq_tx_hdl_t>;

template
class sidekiq_base<skiq_rx_hdl_t>;
