/**
 *
 * @brief Red Pitaya simple signal acquisition utility.
 *
 * (c) Red Pitaya  http://www.redpitaya.com
 *
 * This part of code is written in C programming language.
 * Please visit http://en.wikipedia.org/wiki/C_(programming_language)
 * for more details on the language used herein.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/param.h>
#include <iostream>

#include "rp.h"
#include "rp_hw-calib.h"
#include "version.h"
#include "options.h"
#include "api250-12/rp-spi.h"

/** Program name */
const char *g_argv0 = NULL;


rp_channel_trigger_t getTrigChByTrigSource(rp_acq_trig_src_t src){

    switch(src){
        case RP_TRIG_SRC_NOW:
            return RP_T_CH_1;

        case RP_TRIG_SRC_CHA_PE:
        case RP_TRIG_SRC_CHA_NE:
            return RP_T_CH_1;

        case RP_TRIG_SRC_CHB_PE:
        case RP_TRIG_SRC_CHB_NE:
            return RP_T_CH_2;

        case RP_TRIG_SRC_CHC_PE:
        case RP_TRIG_SRC_CHC_NE:
            return RP_T_CH_3;

        case RP_TRIG_SRC_CHD_PE:
        case RP_TRIG_SRC_CHD_NE:
            return RP_T_CH_4;

        case RP_TRIG_SRC_EXT_PE:
        case RP_TRIG_SRC_EXT_NE:
            return RP_T_CH_EXT;
        default:
            return RP_T_CH_1;
    }
}


typedef struct {
    uint32_t aa;
    uint32_t bb;
    uint32_t pp;
    uint32_t kk;
} ecu_shape_filter_t;

/** Acquire utility main */
int main(int argc, char *argv[])
{

    g_argv0 = argv[0];
    auto option = parse(argc,argv);

    if (option.error || option.showHelp || option.showVersion){
        usage(g_argv0);
        return -1;
    }

    auto model = getModel();
    if (model == RP_250_12){
        if (!option.disableReset) {
            rp_spi_fpga::rp_spi_load_via_fpga("/opt/redpitaya/lib/configs/AD9613BCPZ-250.xml");
        }
    }

    if (rp_InitReset(option.reset_hk) != RP_OK){
        fprintf(stderr,"Error init rp api\n");
        return -1;
    }

    if (rp_CalibInit() != RP_HW_CALIB_OK){
        fprintf(stderr,"Error init calibration\n");
        return -1;
    }

    uint8_t channels = 0;
    if (rp_HPGetFastADCChannelsCount(&channels) != RP_HP_OK){
        fprintf(stderr,"[Error:getRawBuffer] Can't get fast ADC channels count\n");
        return -1;
    }

    rp_calib_params_t calib;
    if (!option.disableCalibration){
        calib = rp_GetCalibrationSettings();
    }else{
        calib = rp_GetDefaultCalibrationSettings();
    }

    if (model == RP_125_14 || model == RP_125_14_4CH){
        if (!option.enableEqualization){
            for(int i = 0; i < calib.fast_adc_count_1_1; ++i){
                calib.fast_adc_filter_1_1[i].aa = 0;
                calib.fast_adc_filter_1_1[i].bb = 0;
            }

            for(int i = 0; i < calib.fast_adc_count_1_20; ++i){
                calib.fast_adc_filter_1_20[i].aa = 0;
                calib.fast_adc_filter_1_20[i].bb = 0;
            }
        }

        if (!option.enableShaping){

            for(int i = 0; i < calib.fast_adc_count_1_1; ++i){
                calib.fast_adc_filter_1_1[i].pp = 0;
                calib.fast_adc_filter_1_1[i].kk = 0xffffff;
            }

            for(int i = 0; i < calib.fast_adc_count_1_20; ++i){
                calib.fast_adc_filter_1_20[i].pp = 0;
                calib.fast_adc_filter_1_20[i].kk = 0xffffff;
            }
        }
    }

    rp_CalibrationSetParams(calib);


    if (rp_HPGetFastADCIsAC_DCOrDefault()){
        for(int i = 0 ; i < channels; i++)
            rp_AcqSetAC_DC((rp_channel_t)i,option.ac_dc_mode[i]);
    }

    for(int i = 0 ; i < channels; i++)
        rp_AcqSetGain((rp_channel_t)i,option.attenuator_mode[i]);

    rp_AcqSetDecimationFactor(option.decimation);

    // Default trigger delay 0 and equal ADC_BUUFER / 2
	rp_AcqSetTriggerDelay(-ADC_BUFFER_SIZE / 2.0 + option.dataSize);
    rp_AcqSetTriggerLevel(getTrigChByTrigSource(option.trigger_mode),option.trigger_level);

	rp_AcqStart();
	rp_AcqSetTriggerSrc(option.trigger_mode);

	bool fillState = false;
	rp_acq_trig_state_t trig_state = RP_TRIG_STATE_WAITING;

	while(1) {
		rp_AcqGetTriggerState(&trig_state);
		if (trig_state == RP_TRIG_STATE_TRIGGERED) {
			break;
		} else {
			usleep(1);
		}
	}

	while(!fillState){
		rp_AcqGetBufferFillState(&fillState);
	}
	rp_AcqStop();

  	uint32_t pos = 0;
    uint32_t acq_u_size = option.dataSize;
	rp_AcqGetWritePointerAtTrig(&pos);

    int start_ch = 0;
    int end_ch = channels - 1;

    if (option.showInVolt){
        float buffers[4][ADC_BUFFER_SIZE];
        for(auto i = start_ch; i <= end_ch; i++){
            auto ch = (rp_channel_t)i;
            rp_AcqGetDataV(ch,pos, &acq_u_size, buffers[i]);

        }
        for(uint32_t i = 0; i< option.dataSize; i++){
            bool printSeparator = false;
            for(auto j = start_ch; j <= end_ch; j++){
                if (printSeparator){
                    fprintf(stdout," ");
                }
                fprintf(stdout,"%f",buffers[j][i]);
                printSeparator = true;
            }
            fprintf(stdout,"\n");
        }

    }else{
        int16_t buffers[4][ADC_BUFFER_SIZE];

        for(auto i = start_ch; i <= end_ch; i++){
            auto ch = (rp_channel_t)i;
            rp_AcqGetDataRaw(ch,pos, &acq_u_size, buffers[i]);
        }

        const char *format_str = (option.showInHex == false) ? "%7d" : "0x%08X";
        for(uint32_t i = 0; i< option.dataSize; i++){
            bool printSeparator = false;
            for(auto j = start_ch; j <= end_ch; j++){
                if (printSeparator){
                    fprintf(stdout," ");
                }
                fprintf(stdout,format_str,buffers[j][i]);
                printSeparator = true;
            }
            fprintf(stdout,"\n");
        }
    }

    if (rp_Release() != RP_OK){
        fprintf(stderr,"Error release rp api\n");
        return -1;
    }
    return 0;
}
