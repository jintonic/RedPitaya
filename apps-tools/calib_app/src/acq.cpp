#include <iostream>
#include <fstream>
#include <chrono>
#include <cassert>
#include <cstring>
#include <vector>
#include "acq.h"
#include "acq_math.h"

// Use (void) to silent unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))
#define ACQ_COUNT 5

COscilloscope::Ptr COscilloscope::Create(uint32_t _decimation){
    return std::make_shared<COscilloscope>(_decimation);
}

COscilloscope::COscilloscope(uint32_t _decimation):
m_decimation(_decimation),
m_index(0),
m_mode(0),
m_channel(RP_CH_1),
m_mutex(),
m_funcSelector(),
m_cursor1(0),
m_cursor2(1),
m_hyst(0.01),
m_channels(0)
{
    m_OscThreadRunState = false;
    m_zoomMode = false;
    m_curCursor1 = m_cursor1;
    m_curCursor2 = m_cursor2;
    m_channels = getADCChannels();
    memset(&m_buffer,0,sizeof(buffers_t));

    for(uint8_t ch = 0; ch < m_channels; ch++){
        m_buffer.ch_i[ch] = new int16_t[ADC_BUFFER_SIZE];
        m_buffer.ch_f[ch] = new float[ADC_BUFFER_SIZE];
    }

}

COscilloscope::~COscilloscope()
{
    stop();
    for(uint8_t ch = 0; ch < m_channels; ch++){
        delete[] m_buffer.ch_i[ch];
        delete[] m_buffer.ch_f[ch];
    }
}

void COscilloscope::stop(){
    m_OscThreadRun.clear();
    if (m_OscThread.joinable()){
        m_OscThread.join();
    }
}

void COscilloscope::start(){
    m_mode = 0;
    startThread();
}

void COscilloscope::startThread(){
    try {
        m_index = 0;
        m_OscThreadRun.test_and_set();
        m_OscThread = std::thread(&COscilloscope::oscWorker, this);
    }
    catch (std::exception& e)
    {
        std::cerr << "Error: COscilloscope::start(), " << e.what() << std::endl;
    }
}

void COscilloscope::startNormal(){
    pthread_mutex_lock(&m_funcSelector);
    m_mode = 0;
    pthread_mutex_unlock(&m_funcSelector);
}

void COscilloscope::startSquare(uint32_t _decimation){
    pthread_mutex_lock(&m_funcSelector);
    m_mode = 1;
    m_decimationSq = _decimation;
    pthread_mutex_unlock(&m_funcSelector);
}

void COscilloscope::startAutoFilter(uint32_t _decimation){
    pthread_mutex_lock(&m_funcSelector);
    m_mode = 2;
    m_decimationSq = _decimation;
    pthread_mutex_unlock(&m_funcSelector);
}

void COscilloscope::startAutoFilterNCh(uint32_t _decimation){
    pthread_mutex_lock(&m_funcSelector);
    m_mode = 3;
    m_decimationSq = _decimation;
    pthread_mutex_unlock(&m_funcSelector);
}

auto COscilloscope::cancel() -> void{
pthread_mutex_lock(&m_funcSelector);
    m_mode = -1;
    pthread_mutex_unlock(&m_funcSelector);
}

void COscilloscope::setAcquireChannel(rp_channel_t _ch){
    pthread_mutex_lock(&m_funcSelector);
    m_channel = _ch;
    pthread_mutex_unlock(&m_funcSelector);
}

void COscilloscope::setHyst(float _value){
    pthread_mutex_lock(&m_funcSelector);
    m_hyst = _value;
    pthread_mutex_unlock(&m_funcSelector);
}

void COscilloscope::updateAcqFilter(rp_channel_t _ch){
    if (rp_HPGetFastADCIsFilterPresentOrDefault()){
        pthread_mutex_lock(&m_funcSelector);
        rp_AcqUpdateAcqFilter(_ch);
        pthread_mutex_unlock(&m_funcSelector);
    }else{
        fprintf(stderr,"[Fatal error] Filter not present in board\n");
    }
}

void COscilloscope::setCursor1(float value){
    m_cursor1 = value;
}
void COscilloscope::setCursor2(float value){
    m_cursor2 = value;
}

void COscilloscope::oscWorker(){
    try{

        while (m_OscThreadRun.test_and_set())
        {
           pthread_mutex_lock(&m_funcSelector);
           if (m_mode == 0){
               acquire();
           }
           if (m_mode == 1){
               acquireSquare();
           }
           if (m_mode == 2){
               acquireAutoFilter();
           }
           if (m_mode == 3){
               acquireAutoFilterSync();
           }
           pthread_mutex_unlock(&m_funcSelector);
           std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }catch (std::exception& e)
    {
        std::cerr << "Error: oscWorker() -> %s\n" << e.what() << std::endl ;
    }
}


void COscilloscope::acquire(){
    uint32_t pos = 0;
    int16_t             timeout = 10000;
    bool                fillState = false;
    uint32_t            acq_u_size = ADC_BUFFER_SIZE;
    uint32_t            acq_u_size_raw = ADC_BUFFER_SIZE;
    rp_acq_trig_state_t trig_state = RP_TRIG_STATE_TRIGGERED;
    rp_AcqSetDecimationFactor(m_decimation);
    rp_AcqSetTriggerDelay( ADC_BUFFER_SIZE / 2.0 );
    rp_AcqStart();
    rp_AcqSetTriggerSrc(RP_TRIG_SRC_NOW);

    for (;timeout > 0;) {
        rp_AcqGetTriggerState(&trig_state);
        if (trig_state == RP_TRIG_STATE_TRIGGERED) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            timeout--;
        }
    }
    while(!fillState && (timeout > 0)){
        rp_AcqGetBufferFillState(&fillState);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        timeout--;
    }
    rp_AcqStop();
    rp_AcqGetWritePointer(&pos);
    m_buffer.size = acq_u_size;
    rp_AcqGetDataV2(pos, &m_buffer);
    m_buffer.size = acq_u_size;
    rp_AcqGetDataRawV2(pos, &m_buffer);

    if (acq_u_size > 0) {
        DataPass localDP;
        localDP.index = m_index++;
        for(auto i = 0u; i < m_channels; i++){
            localDP.ch_avg[i] = 0;
            localDP.ch_max[i] = m_buffer.ch_f[i][0];
            localDP.ch_min[i] = m_buffer.ch_f[i][0];
            localDP.ch_avg_raw[i] = 0;
            localDP.ch_min_raw[i] = m_buffer.ch_i[i][0];
            localDP.ch_max_raw[i] = m_buffer.ch_i[i][0];

            for(auto j = 0u; j < acq_u_size ; ++j){
                if (localDP.ch_max[i] < m_buffer.ch_f[i][j]) localDP.ch_max[i] = m_buffer.ch_f[i][j];
                if (localDP.ch_min[i] > m_buffer.ch_f[i][j]) localDP.ch_min[i] = m_buffer.ch_f[i][j];
                localDP.ch_avg[i] += m_buffer.ch_f[i][j];

            }
            localDP.ch_avg[i] /= (float)acq_u_size;

            for(auto j = 0u ; j < acq_u_size_raw; ++j){
                auto ch = m_buffer.ch_i[i][j];
                if (localDP.ch_max_raw[i] < ch) localDP.ch_max_raw[i] = ch;
                if (localDP.ch_min_raw[i] > ch) localDP.ch_min_raw[i] = ch;
                localDP.ch_avg_raw[i] += ch;
            }
            localDP.ch_avg_raw[i] /= (int32_t)acq_u_size_raw;
        }
        pthread_mutex_lock(&m_mutex);
        m_crossData = localDP;
        pthread_mutex_unlock(&m_mutex);
    }
}

void COscilloscope::acquireSquare(){
    uint32_t pos = 0;
    int16_t             timeout = 1000;
    bool                fillState = false;
    uint32_t            acq_u_size = ADC_BUFFER_SIZE;
    uint32_t            acq_u_size_raw = ADC_BUFFER_SIZE;
    rp_acq_trig_state_t trig_state = RP_TRIG_STATE_TRIGGERED;
    rp_AcqSetDecimationFactor(m_decimationSq);
    rp_AcqSetTriggerDelay( ADC_BUFFER_SIZE/4.0);
    rp_AcqSetTriggerHyst(m_hyst);
    rp_AcqStart();
    uint32_t time = (double)(ADC_BUFFER_SIZE / 4) * ((double)m_decimationSq / getADCRate()) * 1000000;
    std::this_thread::sleep_for(std::chrono::microseconds(time == 0 ? 1 : time));
    switch(m_channel){
        case RP_CH_1:
            rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHA_PE);
        break;
        case RP_CH_2:
            rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHB_PE);
        break;
        case RP_CH_3:
            rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHC_PE);
        break;
        case RP_CH_4:
            rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHD_PE);
        break;
        default:
            assertm(false, "ERROR: void COscilloscope::acquireSquare() - Unknown channel");
    }
    for (;timeout > 0;) {
        rp_AcqGetTriggerState(&trig_state);
        if (trig_state == RP_TRIG_STATE_TRIGGERED) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            timeout--;
        }
    }
    if (timeout == 0){
        rp_AcqSetTriggerSrc(RP_TRIG_SRC_NOW);
        timeout = 1000;
    }
    while(!fillState && (timeout > 0)){
        rp_AcqGetBufferFillState(&fillState);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        timeout--;
    }
    rp_AcqStop();
 //   rp_AcqGetWritePointerAtTrig(&pos);
    rp_AcqGetWritePointer(&pos);
    float buffer[ADC_BUFFER_SIZE];
    rp_AcqGetDataV(m_channel,(pos + 1)  % ADC_BUFFER_SIZE , &acq_u_size, buffer);

//  rp_AcqGetDataV2((pos + 1)  % ADC_BUFFER_SIZE , &acq_u_size, m_buffer[0], m_buffer[1]);
//  rp_AcqGetDataRawV2((pos + 1) % ADC_BUFFER_SIZE, &acq_u_size_raw, m_buffer_raw[0] , m_buffer_raw[1]);

    if (acq_u_size > 0) {
        auto *ch = buffer;
        //ch = filterBuffer(ch,acq_u_size);
        m_curCursor1 = std::min(m_curCursor1,m_curCursor2);
        m_curCursor2 = std::max(m_curCursor1,m_curCursor2);
        auto min_cursor = std::min(m_cursor1,m_cursor2);
        auto max_cursor = std::max(m_cursor1,m_cursor2);
        double min_dt = min_cursor;
        double max_dt = 1 - max_cursor;

        auto time  = duration_cast< microseconds >(system_clock::now().time_since_epoch());
        auto time_diff = (time - m_lastTimeAni);
        double dt = static_cast<double>(time_diff.count()) / TIME_ANIMATION;
        min_dt *= dt;
        max_dt *= dt;
        m_lastTimeAni = time;

        if (m_zoomMode){
            m_curCursor1 += min_dt;
            m_curCursor2 -= max_dt;
            if (m_curCursor1 > min_cursor) { m_curCursor1 = min_cursor; m_curCursor2 = max_cursor; }
            if (m_curCursor2 < max_cursor) { m_curCursor1 = min_cursor; m_curCursor2 = max_cursor; }
        }else{
            m_curCursor1 -= min_dt;
            m_curCursor2 += max_dt;
            if (m_curCursor1 < 0 || m_curCursor1 > 1) { m_curCursor1 = 0; m_curCursor2 = 1; }
            if (m_curCursor2 > 1 || m_curCursor2 < 0) { m_curCursor1 = 0; m_curCursor2 = 1; }
        }
        DataPassSq localDP = selectRange(ch, m_curCursor1 , m_curCursor2);
        pthread_mutex_lock(&m_mutex);
        m_crossDataSq = localDP;
        pthread_mutex_unlock(&m_mutex);
        //delete ch;
    }
}

COscilloscope::DataPassSq COscilloscope::selectRange(float *buffer,double _start,double _stop){
    DataPassSq localDP;
    localDP.index = m_index++;
    localDP.cur_channel = m_channel;
    int startX = (double)ADC_BUFFER_SIZE * std::min(_start,_stop);
    int stopX  = (double)ADC_BUFFER_SIZE * std::max(_start,_stop);
    float core_size = (float)(stopX - startX) / (float)SCREEN_BUFF_SIZE;
  //  int screenSize = core_size < 1 ? stopX - startX : SCREEN_BUFF_SIZE;
    if (core_size < 1){
        localDP.wave_size = stopX - startX;
        int j = 0;
        for( int i = startX ; i < stopX ;i ++){
            localDP.wave[j++] = buffer[i];
        }
    }else{
        localDP.wave_size = SCREEN_BUFF_SIZE;
        for( int i = 0 ; i < SCREEN_BUFF_SIZE ;i ++){
            float sum = 0;
            int ii = (float)i * core_size + startX;
            for( int j = 0 ; j < (int)core_size ; j++){
                sum += (float)(buffer[ ii + j ]);
            }
            sum /= (int)core_size;
            localDP.wave[i] = sum;
        }
    }
    return localDP;
}


void COscilloscope::acquireAutoFilter(){
    if (!rp_HPGetFastADCIsFilterPresentOrDefault()){
        fprintf(stderr,"[Fatal error] Filter not present in board\n");
        exit(-1);
    }

    DataPassAutoFilter localDP;
    uint32_t            pos = 0;
    int16_t             timeout = 1000000; // timeout 1 second
    int16_t             repeat_count = 0;
    bool                fillState = false;
    uint32_t            aa,bb,pp,kk;
    uint32_t            acq_u_size = ADC_BUFFER_SIZE;
    uint32_t            acq_u_size_raw = ADC_BUFFER_SIZE;
    float               m_acu_buffer[ADC_BUFFER_SIZE];
    float               m_acu_buffer_raw[ADC_BUFFER_SIZE];
    memset(m_acu_buffer,0,sizeof(float) * ADC_BUFFER_SIZE);
    memset(m_acu_buffer_raw,0,sizeof(float) * ADC_BUFFER_SIZE);
    rp_acq_trig_state_t trig_state = RP_TRIG_STATE_TRIGGERED;
    localDP.ampl = -1;
    localDP.is_valid = false;
    rp_AcqGetFilterCalibValue(m_channel,&localDP.f_aa,&localDP.f_bb,&localDP.f_kk,&localDP.f_pp);
    while(repeat_count < ACQ_COUNT) {
        timeout = 1000000;
        fillState = false;
        trig_state = RP_TRIG_STATE_TRIGGERED;
        rp_AcqSetDecimationFactor(m_decimationSq);
        rp_AcqSetTriggerDelay( ADC_BUFFER_SIZE/4.0);
        rp_AcqSetTriggerHyst(m_hyst);
        rp_AcqGetFilterCalibValue(m_channel,&aa,&bb,&kk,&pp);
        rp_AcqSetTriggerSrc(RP_TRIG_SRC_DISABLED);
        rp_AcqStart();
        uint32_t time = (double)(ADC_BUFFER_SIZE / 4) * ((double)m_decimationSq / getADCRate()) * 1000000;
        std::this_thread::sleep_for(std::chrono::microseconds(time == 0 ? 1 : time));
        switch(m_channel){
            case RP_CH_1:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHA_PE);
            break;
            case RP_CH_2:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHB_PE);
            break;
            case RP_CH_3:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHC_PE);
            break;
            case RP_CH_4:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHD_PE);
            break;
            default:
                assertm(false, "ERROR: void COscilloscope::acquireSquare() - Unknown channel");

        }
        for (;timeout > 0;) {
            rp_AcqGetTriggerState(&trig_state);
            if (trig_state == RP_TRIG_STATE_TRIGGERED) {
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                timeout--;
            }
        }

        while(!fillState && (timeout > 0)){
            rp_AcqGetBufferFillState(&fillState);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            timeout--;
        }
        rp_AcqStop();
        if (timeout <= 0 ) return;
    //   rp_AcqGetWritePointerAtTrig(&pos);
        rp_AcqGetWritePointer(&pos);
        rp_AcqGetDataV(m_channel,(pos + 1)  % ADC_BUFFER_SIZE , &acq_u_size, m_buffer.ch_f[m_channel]);
        rp_AcqGetDataRaw(m_channel,(pos + 1)  % ADC_BUFFER_SIZE , &acq_u_size, m_buffer.ch_i[m_channel]);

        // rp_AcqGetDataV2((pos + 1)  % ADC_BUFFER_SIZE , &acq_u_size, m_buffer[0], m_buffer[1]);
        // rp_AcqGetDataRawV2((pos + 1) % ADC_BUFFER_SIZE, &acq_u_size_raw, m_buffer_raw[0] , m_buffer_raw[1]);

        if (aa != localDP.f_aa || bb != localDP.f_bb || pp != localDP.f_pp || kk != localDP.f_kk) return;

        auto *ch = m_buffer.ch_f[m_channel];
        auto *ch_raw = m_buffer.ch_i[m_channel];
        for(int i = 0 ; i < acq_u_size ; i++){
            m_acu_buffer[i] += ch[i];
            m_acu_buffer_raw[i] += ch_raw[i];
        }
        repeat_count++;
    }

    for(int i = 0 ; i < acq_u_size ; i++){
        m_acu_buffer[i] /= (double)repeat_count;
        m_acu_buffer_raw[i] /= (double)repeat_count;
    }
    localDP.cur_channel = m_channel;
    localDP.index = m_index++;
    auto cross = calcCountCrossZero(m_acu_buffer,acq_u_size);
    if (cross.size() >= 2 ){
        auto last_max = findLastMax(m_acu_buffer,acq_u_size,cross[1]);
        auto last_max_raw = findLastMax(m_acu_buffer_raw,acq_u_size,cross[1]);
        double value = calculate(m_acu_buffer, acq_u_size, m_acu_buffer[last_max], cross[0],cross[1],localDP.deviation);
        double value_raw = calculate(m_acu_buffer_raw, acq_u_size, m_acu_buffer_raw[last_max], cross[0],cross[1],localDP.deviation);
        localDP.is_valid = true;
        localDP.calib_value = value;
        localDP.calib_value_raw = value_raw;
        localDP.ampl = m_acu_buffer[last_max];
      //  std::cout << m_acu_buffer[last_max] << std::endl;
    }
    pthread_mutex_lock(&m_mutex);
    m_crossDataAutoFilter = localDP;
    pthread_mutex_unlock(&m_mutex);

}

void COscilloscope::acquireAutoFilterSync(){

    if (!rp_HPGetFastADCIsFilterPresentOrDefault()){
        fprintf(stderr,"[Fatal error] Filter not present in board\n");
        exit(-1);
    }

    DataPassAutoFilterSync localDP;
    uint32_t            pos = 0;
    int16_t             timeout = 1000000; // timeout 1 second
    int16_t             repeat_count = 0;
    bool                fillState = false;
    uint32_t            aa[MAX_ADC_CHANNELS],bb[MAX_ADC_CHANNELS],pp[MAX_ADC_CHANNELS],kk[MAX_ADC_CHANNELS];
    uint32_t            acq_u_size = ADC_BUFFER_SIZE;
    uint32_t            acq_u_size_raw = ADC_BUFFER_SIZE;
    float               m_acu_buffer[MAX_ADC_CHANNELS][ADC_BUFFER_SIZE];
    float               m_acu_buffer_raw[MAX_ADC_CHANNELS][ADC_BUFFER_SIZE];
    memset(m_acu_buffer,0,sizeof(float) * ADC_BUFFER_SIZE * MAX_ADC_CHANNELS);
    memset(m_acu_buffer_raw,0,sizeof(float) * ADC_BUFFER_SIZE * MAX_ADC_CHANNELS);
    rp_acq_trig_state_t trig_state = RP_TRIG_STATE_TRIGGERED;

    for(auto i = 0u; i < m_channels; i++){
        localDP.valueCH[i].ampl = -1;
        localDP.valueCH[i].is_valid = false;
        rp_AcqGetFilterCalibValue((rp_channel_t)i,&localDP.valueCH[i].f_aa,&localDP.valueCH[i].f_bb,&localDP.valueCH[i].f_kk,&localDP.valueCH[i].f_pp);
    }

    while(repeat_count < ACQ_COUNT) {
        timeout = 1000000;
        fillState = false;
        trig_state = RP_TRIG_STATE_TRIGGERED;
        rp_AcqSetDecimationFactor(m_decimationSq);
        rp_AcqSetTriggerDelay( ADC_BUFFER_SIZE/4.0);
        rp_AcqSetTriggerHyst(m_hyst);
        for(auto i = 0u; i < m_channels; i++){
            rp_AcqGetFilterCalibValue((rp_channel_t)i,&aa[i],&bb[i],&kk[i],&pp[i]);
        }
        rp_AcqSetTriggerSrc(RP_TRIG_SRC_DISABLED);
        rp_AcqStart();
        uint32_t time = (double)(ADC_BUFFER_SIZE / 4) * ((double)m_decimationSq / getADCRate()) * 1000000;
        std::this_thread::sleep_for(std::chrono::microseconds(time == 0 ? 1 : time));
        switch(m_channel){
            case RP_CH_1:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHA_PE);
            break;
            case RP_CH_2:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHB_PE);
            break;
            case RP_CH_3:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHC_PE);
            break;
            case RP_CH_4:
                rp_AcqSetTriggerSrc(RP_TRIG_SRC_CHD_PE);
            break;
            default:
                assertm(false, "ERROR: void COscilloscope::acquireSquare() - Unknown channel");

        }
        for (;timeout > 0;) {
            rp_AcqGetTriggerState(&trig_state);
            if (trig_state == RP_TRIG_STATE_TRIGGERED) {
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                timeout--;
            }
        }

        while(!fillState && (timeout > 0)){
            rp_AcqGetBufferFillState(&fillState);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            timeout--;
        }
        rp_AcqStop();
        if (timeout <= 0 ) return;
    //   rp_AcqGetWritePointerAtTrig(&pos);
        rp_AcqGetWritePointer(&pos);
        m_buffer.size = acq_u_size;
        rp_AcqGetDataV2((pos + 1)  % ADC_BUFFER_SIZE , &m_buffer);
        m_buffer.size = acq_u_size;
        rp_AcqGetDataRawV2((pos + 1) % ADC_BUFFER_SIZE, &m_buffer);

        bool exitFlag = true;
        for(auto i = 0u; i < m_channels; i++){
            exitFlag &= (aa[i] != localDP.valueCH[i].f_aa || bb[i] != localDP.valueCH[i].f_bb || pp[i] != localDP.valueCH[i].f_pp || kk[i] != localDP.valueCH[i].f_kk);
        }
        if (exitFlag) return;
        for(auto j = 0u; j < m_channels; j++){
            for(int i = 0 ; i < acq_u_size ; i++){
                m_acu_buffer[j][i] += m_buffer.ch_f[j][i];
                m_acu_buffer_raw[j][i] += m_buffer.ch_i[j][i];
            }
        }
        repeat_count++;
    }
    for(auto j = 0u; j < m_channels; j++){
        for(int i = 0 ; i < acq_u_size ; i++){
            m_acu_buffer[j][i] /= (double)repeat_count;
            m_acu_buffer_raw[j][i] /= (double)repeat_count;
        }
    }
    for(auto j = 0u; j < m_channels; j++){
        localDP.valueCH[j].cur_channel = (rp_channel_t)j;
        localDP.valueCH[j].index = m_index;
    }
    m_index++;

    for(auto j = 0u; j < m_channels; j++){
        auto cross1 = calcCountCrossZero(m_acu_buffer[j],acq_u_size);
        if (cross1.size() >= 2){
            auto last_max1     = findLastMax(m_acu_buffer[j],acq_u_size,cross1[1]);
            auto last_max_raw1 = findLastMax(m_acu_buffer_raw[j],acq_u_size,cross1[1]);
            double value1 = calculate(m_acu_buffer[j], acq_u_size, m_acu_buffer[j][last_max1], cross1[0],cross1[1],localDP.valueCH[j].deviation);
            double value_raw1 = calculate(m_acu_buffer_raw[j], acq_u_size, m_acu_buffer_raw[j][last_max_raw1], cross1[0],cross1[1],localDP.valueCH[j].deviation);
            localDP.valueCH[j].is_valid = true;
            localDP.valueCH[j].calib_value = value1;
            localDP.valueCH[j].calib_value_raw = value_raw1;
            localDP.valueCH[j].ampl = m_acu_buffer[j][last_max1];
            // std::cout << "\n" << last_max1 << " - " << m_acu_buffer[0][last_max1] << std::endl;
            // std::cout << "\n" << last_max2 << " - " <<  m_acu_buffer[1][last_max2] << std::endl;
        }
    }
    pthread_mutex_lock(&m_mutex);
    m_crossDataAutoFilterSync = localDP;
    pthread_mutex_unlock(&m_mutex);
}

void COscilloscope::setZoomMode(bool enable){
    m_zoomMode = enable;
    m_lastTimeAni = duration_cast< microseconds >(system_clock::now().time_since_epoch());
}


void COscilloscope::setLV(){
    for(auto j = 0u; j < m_channels; j++){
        rp_AcqSetGain((rp_channel_t)j, RP_LOW);
    }
}

void COscilloscope::setHV(){
    for(auto j = 0u; j < m_channels; j++){
        rp_AcqSetGain((rp_channel_t)j, RP_HIGH);
    }
}

COscilloscope::DataPass COscilloscope::getData(){
    DataPass local_pass;
    pthread_mutex_lock(&m_mutex);
    local_pass = m_crossData;
    pthread_mutex_unlock(&m_mutex);
    return local_pass;
}

COscilloscope::DataPassSq COscilloscope::getDataSq(){
    DataPassSq local_pass;
    pthread_mutex_lock(&m_mutex);
    local_pass = m_crossDataSq;
    pthread_mutex_unlock(&m_mutex);
    return local_pass;
}

COscilloscope::DataPassAutoFilter COscilloscope::getDataAutoFilter(){
    DataPassAutoFilter local_pass;
    pthread_mutex_lock(&m_mutex);
    local_pass = m_crossDataAutoFilter;
    pthread_mutex_unlock(&m_mutex);
    return local_pass;
}

COscilloscope::DataPassAutoFilterSync COscilloscope::getDataAutoFilterSync(){
    DataPassAutoFilterSync local_pass;
    pthread_mutex_lock(&m_mutex);
    local_pass = m_crossDataAutoFilterSync;
    pthread_mutex_unlock(&m_mutex);
    return local_pass;
}


void COscilloscope::setGEN_DISABLE(){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setGEN_DISABLE] Fast DAC not present on board\n");
        exit(-1);
    }
    rp_GenOutDisable(RP_CH_1);
    rp_GenOutDisable(RP_CH_2);
}

void COscilloscope::setGEN0(){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setGEN0] Fast DAC not present on board\n");
        exit(-1);
    }
    rp_GenAmp(RP_CH_1, 0);
    rp_GenAmp(RP_CH_2, 0);
	rp_GenOffset(RP_CH_1, 0);
    rp_GenOffset(RP_CH_2, 0);
    rp_GenWaveform(RP_CH_1, RP_WAVEFORM_DC);
    rp_GenWaveform(RP_CH_2, RP_WAVEFORM_DC);
    rp_GenFreq(RP_CH_1, 100);
    rp_GenFreq(RP_CH_2, 100);
    rp_GenRiseTime(RP_CH_1,0);
    rp_GenRiseTime(RP_CH_2,0);
    rp_GenFallTime(RP_CH_1,0);
    rp_GenFallTime(RP_CH_2,0);
    rp_GenOutEnable(RP_CH_1);
    rp_GenOutEnable(RP_CH_2);
}

void COscilloscope::setGEN0_5(){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setGEN0_5] Fast DAC not present on board\n");
        exit(-1);
    }
    rp_GenAmp(RP_CH_1, 0.5);
    rp_GenAmp(RP_CH_2, 0.5);
	rp_GenOffset(RP_CH_1, 0);
    rp_GenOffset(RP_CH_2, 0);
    rp_GenWaveform(RP_CH_1, RP_WAVEFORM_DC);
    rp_GenWaveform(RP_CH_2, RP_WAVEFORM_DC);
    rp_GenFreq(RP_CH_1, 100);
    rp_GenFreq(RP_CH_2, 100);
    rp_GenRiseTime(RP_CH_1,0);
    rp_GenRiseTime(RP_CH_2,0);
    rp_GenFallTime(RP_CH_1,0);
    rp_GenFallTime(RP_CH_2,0);
    rp_GenOutEnable(RP_CH_1);
    rp_GenOutEnable(RP_CH_2);
}

void COscilloscope::setGEN0_5_SINE(){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setGEN0_5_SINE] Fast DAC not present on board\n");
        exit(-1);
    }
    rp_GenAmp(RP_CH_1, 0.5);
    rp_GenAmp(RP_CH_2, 0.5);
	rp_GenOffset(RP_CH_1, 0);
    rp_GenOffset(RP_CH_2, 0);
    rp_GenWaveform(RP_CH_1, RP_WAVEFORM_SINE);
    rp_GenWaveform(RP_CH_2, RP_WAVEFORM_SINE);
    rp_GenFreq(RP_CH_1, 2500);
    rp_GenFreq(RP_CH_2, 2500);
    rp_GenRiseTime(RP_CH_1,0);
    rp_GenRiseTime(RP_CH_2,0);
    rp_GenFallTime(RP_CH_1,0);
    rp_GenFallTime(RP_CH_2,0);
    rp_GenOutEnable(RP_CH_1);
    rp_GenOutEnable(RP_CH_2);
}


void COscilloscope::setDC(){
    if (!rp_HPGetFastADCIsAC_DCOrDefault()){
        fprintf(stderr,"[Error:setDC] AC/DC mode not present on board\n");
        exit(-1);
    }
    rp_AcqSetAC_DC(RP_CH_1,RP_DC);
    rp_AcqSetAC_DC(RP_CH_2,RP_DC);
}

void COscilloscope::setAC(){
    if (!rp_HPGetFastADCIsAC_DCOrDefault()){
        fprintf(stderr,"[Error:setDC] AC/DC mode not present on board\n");
        exit(-1);
    }
    rp_AcqSetAC_DC(RP_CH_1,RP_AC);
    rp_AcqSetAC_DC(RP_CH_2,RP_AC);
}

void COscilloscope::setGenGainx1(){
    if (!rp_HPGetIsGainDACx5OrDefault()){
        fprintf(stderr,"[Error:setDC] Gen gain mode not present on board\n");
        exit(-1);
    }
    rp_GenSetGainOut(RP_CH_1,RP_GAIN_1X);
    rp_GenSetGainOut(RP_CH_2,RP_GAIN_1X);
}

void COscilloscope::setGenGainx5(){
    if (!rp_HPGetIsGainDACx5OrDefault()){
        fprintf(stderr,"[Error:setDC] Gen gain mode not present on board\n");
        exit(-1);
    }
    rp_GenSetGainOut(RP_CH_1,RP_GAIN_5X);
    rp_GenSetGainOut(RP_CH_2,RP_GAIN_5X);
}

void COscilloscope::resetGen(){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:resetGen] Fast DAC not present on board\n");
        exit(-1);
    }
    enableGen(RP_CH_1,false);
    enableGen(RP_CH_2,false);
    rp_GenWaveform(RP_CH_1, RP_WAVEFORM_SINE);
    rp_GenWaveform(RP_CH_2, RP_WAVEFORM_SINE);
    setFreq(RP_CH_1,1000);
    setFreq(RP_CH_2,1000);
    rp_GenRiseTime(RP_CH_1,0);
    rp_GenRiseTime(RP_CH_2,0);
    rp_GenFallTime(RP_CH_1,0);
    rp_GenFallTime(RP_CH_2,0);
    setAmp(RP_CH_1,0.9);
    setAmp(RP_CH_2,0.9);
    setOffset(RP_CH_1,0);
    setOffset(RP_CH_2,0);
}

void COscilloscope::enableGen(rp_channel_t _ch,bool _enable){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:enableGen] Fast DAC not present on board\n");
        exit(-1);
    }
    if (_enable){
        rp_GenOutEnable(_ch);
        rp_GenResetTrigger(_ch);
    }else{
        rp_GenOutDisable(_ch);
    }
}

int COscilloscope::setFreq(rp_channel_t _ch,int _freq){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setFreq] Fast DAC not present on board\n");
        exit(-1);
    }
    return rp_GenFreq(_ch,_freq);
}

int COscilloscope::setAmp(rp_channel_t _ch,float _ampl){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setAmp] Fast DAC not present on board\n");
        exit(-1);
    }
    return rp_GenAmp(_ch,_ampl);
}

int COscilloscope::setOffset(rp_channel_t _ch,float _offset){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setOffset] Fast DAC not present on board\n");
        exit(-1);
    }
    return rp_GenOffset(_ch,_offset);
}

int COscilloscope::setGenType(rp_channel_t _ch,int _type){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:setGenType] Fast DAC not present on board\n");
        exit(-1);
    }
    return rp_GenWaveform(_ch, (rp_waveform_t)_type);
}

void COscilloscope::updateGenCalib(){
    if (getDACChannels() < 2){
        fprintf(stderr,"[Error:updateGenCalib] Fast DAC not present on board\n");
        exit(-1);
    }
    float x = 0;
    rp_GenGetAmp(RP_CH_1,&x);
    rp_GenAmp(RP_CH_1,x);
    rp_GenGetAmp(RP_CH_2,&x);
    rp_GenAmp(RP_CH_2,x);
    rp_GenGetOffset(RP_CH_1,&x);
    rp_GenOffset(RP_CH_1,x);
    rp_GenGetOffset(RP_CH_2,&x);
    rp_GenOffset(RP_CH_2,x);
}