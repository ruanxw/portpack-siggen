/*
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ui_sig_gen_app.hpp"
#include "string_format.hpp"

#include "ui_fileman.hpp"
#include "io_file.hpp"
#include "metadata_file.hpp"
#include "utility.hpp"
#include "file_path.hpp"
#include "file_reader.hpp"

#include "baseband_api.hpp"
#include "portapack.hpp"
#include "portapack_persistent_memory.hpp"

#include <codecvt>
#include <locale>

#include "usb_serial_asyncmsg.hpp"

using namespace portapack;
namespace fs = std::filesystem;

namespace ui {

void SigGenAppView::set_ready() {
    ready_signal = true;
}

void SigGenAppView::on_file_changed(const fs::path& new_file_path) {
    file_path = new_file_path;
    File::Size file_size{};

    {  // Get the size of the data file.
        File data_file;
        auto error = data_file.open(file_path);
        if (error) {
            file_error();
            return;
        }

        file_size = data_file.size();
    }

    // Get original record frequency if available.
    auto metadata_path = get_metadata_path(file_path);
    auto metadata = read_metadata_file(metadata_path);

    if (metadata) {
        if(0 == metadata->center_frequency)
        {
            metadata->center_frequency = transmitter_model.target_frequency();
        }
        field_frequency.set_value(metadata->center_frequency);
        transmitter_model.set_sampling_rate(metadata->sample_rate);
    }

    transmitter_model.set_baseband_bandwidth(1'750'000);

    // UI Fixup.
    text_sample_rate.set(unit_auto_scale(transmitter_model.sampling_rate(), 3, 1) + "Hz");
    progressbar.set_max(file_size);
    text_filename.set(truncate(file_path.filename().string(), 12));

    auto duration = ms_duration(file_size, transmitter_model.sampling_rate(), 2);
    text_duration.set(to_string_time_ms(duration));

    // TODO: fix in UI framework with 'try_focus()'?
    // Hack around focus getting called by ctor before parent is set.
    if (parent())
        button_play.focus();
}

void SigGenAppView::on_tx_progress(const uint32_t progress) {
    progressbar.set_value(progress);
}

void SigGenAppView::focus() {
    button_load_last_config.focus();
    button_open.focus();
}

void SigGenAppView::file_error() {
    nav_.display_modal("Error", "File read error.");
}

void SigGenAppView::config_file_error() {
    nav_.display_modal("Error", "config File read error.");
}

bool SigGenAppView::is_active() const {
    return (bool)replay_thread;
}

void SigGenAppView::toggle() {
    if (is_transmitting) {
        stop_cyclic();
        stop(false);
        is_transmitting = FALSE;
        button_play.set_bitmap(&bitmap_play);
    } else {
        save_last_config();   //保存预计发送的文件，用于下次开机能够自动加载前一次发射的文件。
        if(check_cycle_enable.value() && field_cycle_pause.value() != 0){    //如果开启了间隔发射模式则进入间隔发射流程
            cyclic_tx_ctr(TRUE);
        }
        else{
            start();
        }
        if(is_active())
        {
            is_transmitting = TRUE;
            button_play.set_bitmap(&bitmap_stop);
        }
        
    }
}

void SigGenAppView::start() {
    std::unique_ptr<stream::Reader> reader;

    auto p = std::make_unique<FileReader>();
    auto open_error = p->open(file_path);
    if (open_error.is_valid()) {
        file_error();
        return;
    } else {
        reader = std::move(p);
    }

    if (reader) {
        replay_thread = std::make_unique<ReplayThread>(
            std::move(reader),
            read_size, buffer_count,
            &ready_signal,
            [](uint32_t return_code) {
                ReplayThreadDoneMessage message{return_code};
                EventDispatcher::send_message(message);
            });
    }

    transmitter_model.enable();
}

void SigGenAppView::stop(const bool do_loop) {
    transmitter_model.disable();
    if (is_active())
        replay_thread.reset();
    //1.doloop为真，文件读完了，没有开启循环发送时，就直接结束。这时需要更新UI,直接触发toggle就可以。（只有文件读完，doloop为true）
    //2.doloop为真，文件读完了，开启了循环发送，这个时候判断是否开启了间隔发送，间隔发送用cycle_pause，来判断，如没有开启间隔发送，则持续性循环发送，则进入start()。
    //3.doloop为真，文件读完了，开启了循环发送，这个时候判断是否开启了间隔发送，间隔发送用cycle_pause，来判断，如开启间隔发送，撒也不干，因为下次开始发数据是定时器来调。
    //4.doloop为假，既不需要更新ui，也不需要重新开始。
    if (do_loop && !check_cycle_enable.value()) {  //一次性发送
        toggle();
    }else if(do_loop && check_cycle_enable.value()){
        if(field_cycle_pause.value() == 0){   //长发
            start();
        }        
    } 

    ready_signal = false;
}

void SigGenAppView::handle_replay_thread_done(const uint32_t return_code) {
    if (return_code == ReplayThread::END_OF_FILE) {
        stop(true);
    } else if (return_code == ReplayThread::READ_ERROR) {
        stop(false);
        stop_cyclic();
        file_error();
    }

    progressbar.set_value(0);
}

SigGenAppView::SigGenAppView(
    NavigationView& nav)
    : nav_(nav) {
    baseband::run_image(portapack::spi_flash::image_tag_sig_gen);
    //baseband::run_prepared_image(portapack::memory::map::m4_code.base());

    add_children({
        &button_open,
        &button_load_last_config,
        &text_filename,
        &text_sample_rate,
        &text_duration,
        &progressbar,
        &field_frequency,
        &tx_view,  // now it handles previous rfgain, rfamp.
        //&check_loop,
        &button_play,
        &check_cycle_enable,
        &text_cycle_tx,
        &field_cycle_tx,
        &text_cycle_pause,
        &field_cycle_pause
        //&waterfall,
    });

    field_frequency.set_step(5000);

    button_play.on_select = [this](ImageButton&) {
        this->toggle();
    };

    button_open.on_select = [this, &nav](Button&) {
        auto open_view = nav.push<FileLoadView>(".C8");
        ensure_directory(sig_gen_dir);
        open_view->push_dir(sig_gen_dir);
        open_view->on_changed = [this](std::filesystem::path new_file_path) {
            on_file_changed(new_file_path);
        };
    };

    //enable为1，pause为0，则默认开启长发。  tx的值不为0就行。
    check_cycle_enable.set_value(TRUE);
    field_cycle_tx.set_value(5);
    field_cycle_pause.set_value(0);   

    button_load_last_config.on_select = [this, &nav](Button&) {
        load_last_config();
    };
}

void SigGenAppView::load_last_config(){
    File config_file;
    
    auto error = config_file.open(config_file_name);
    if (!error) {
        auto reader = FileLineReader(config_file);
        uint32_t i = 0;
        std::wstring_convert<std::codecvt_utf8_utf16<std::filesystem::path::value_type>, std::filesystem::path::value_type> conv;
        for (const auto& line : reader) {
            if (line.length() == 0 || line[0] == '#'  || line[0] == '\r' || line[0] == '\n')
                continue;
            UsbSerialAsyncmsg::asyncmsg("i = >>>");
            UsbSerialAsyncmsg::asyncmsg(i);
            UsbSerialAsyncmsg::asyncmsg(line);
            switch(i++){
                case 0:   //第一行是IQ文件
                    //std::wstring_convert<std::codecvt_utf8_utf16<std::filesystem::path::value_type>, std::filesystem::path::value_type> conv;
                    //std::filesystem::path sigwave_path = conv.from_bytes(line.c_str());
                    on_file_changed(conv.from_bytes(line.c_str()));
                    break;
/*                case 1: //第二行是频率
                    //field_frequency.set_value(std::stoull(line));
                    break;
                
                 case 2: //第三行是gain
                    break;

                case 3: //第四行是AMP
                    break;
 */
                case 1: //第五行是cycle tx enable
                    check_cycle_enable.set_value(static_cast<bool>(std::stoi(line.c_str())));
                    //UsbSerialAsyncmsg::asyncmsg(static_cast<bool>(std::stoi(line.c_str())));
                    //UsbSerialAsyncmsg::asyncmsg(line == "1");
                    break;

                case 2: //第六行是cycle tx time
                    field_cycle_tx.set_value(static_cast<int32_t>(std::stoi(line.c_str())));
                    UsbSerialAsyncmsg::asyncmsg(static_cast<uint32_t>(std::stoi(line.c_str())));
                    break;

                case 3: //第五行是cycle pause time
                    field_cycle_pause.set_value(static_cast<int32_t>(std::stoi(line.c_str())));
                    UsbSerialAsyncmsg::asyncmsg(static_cast<uint32_t>(std::stoi(line.c_str())));
                    break;

            }        
    }
    return;
/*     auto result = File::read_file(config_file_name);
    if(result.is_ok())
    {
        std::wstring_convert<std::codecvt_utf8_utf16<std::filesystem::path::value_type>, std::filesystem::path::value_type> conv;
        std::filesystem::path sigwave_path = conv.from_bytes(result.value().c_str());
        on_file_changed(sigwave_path);
    } */
}

void SigGenAppView::save_last_config(){
    File config_file;
    std::string config_content = "";
    delete_file(config_file_name);
    auto error_open = config_file.open(config_file_name, false, true);
    if (error_open)
        return;

    config_content += file_path.string();
    config_content += "\r\n";   //第一行为IQ文件
    //config_content += std::to_string(field_frequency.value());
    //config_content += to_string_dec_int(field_frequency.value());
    //config_content += "\r\n";   //第二行为频率
    //config_content += std::to_string(tx_view.field_gain.value()); //tx_view.field_gain.value().string();
    //config_content += "\r\n";   //第三行为gain
    //config_content += std::to_string(tx_view.field_amp.value());  //tx_view.field_amp.value().string();
    //config_content += "\r\n";   //第四行为Amp
    config_content += std::to_string(check_cycle_enable.value());  //check_cycle_enable.value().string();
    config_content += "\r\n";   //第五行为循环发送开关
    config_content += std::to_string(field_cycle_tx.value());  //field_cycle_tx.value().string();
    config_content += "\r\n";   //第六行为循环发送中的发射时间
    config_content += std::to_string(field_cycle_pause.value()); //field_cycle_pause.value().string();
    config_content += "\r\n";   //第七行为循环发送中的暂停时间
    UsbSerialAsyncmsg::asyncmsg(config_content);

    auto error_write = config_file.write(config_content.c_str(), config_content.size());
    config_file.close();
}

void SigGenAppView::stop_cyclic() {
    if(is_cycle_timer_enabled)
        chVTReset(&cycle_timer);
        is_cycle_timer_enabled = FALSE;
}

void SigGenAppView::cycle_cb(void* arg) {
    auto obj = static_cast<SigGenAppView*>(arg);
    CyclicTXCtrMessage message;
    message.CyclicTXCtr = obj->is_active() ? FALSE : TRUE;
    EventDispatcher::send_message_from_isr(message);
    return;
}

//CyclicTXCtr: TRUE tx start; FALSE tx pause;
void SigGenAppView::cyclic_tx_ctr(bool CyclicTXCtr){
    std::string cyclic_tx_ctr = "cyclic_tx_ctr function:";
    std::string tx_start = "cyclic_tx_ctr: tx enable";  
    std::string tx_pause = "cyclic_tx_ctr: tx pause";
    UsbSerialAsyncmsg::asyncmsg(cyclic_tx_ctr);
    if(CyclicTXCtr){
        UsbSerialAsyncmsg::asyncmsg(tx_start);
        chVTSet(&cycle_timer, S2ST(field_cycle_tx.value()), cycle_cb, this);
        start(); // 开始信号生成
    }else{
        UsbSerialAsyncmsg::asyncmsg(tx_pause);
        chVTSet(&cycle_timer, S2ST(field_cycle_pause.value()), cycle_cb, this);
        stop(false); // 开始信号生成
    }
    is_cycle_timer_enabled = TRUE;
}

SigGenAppView::~SigGenAppView() {
    stop_cyclic();
    transmitter_model.disable();
    baseband::shutdown();
}

void SigGenAppView::on_hide() {
    // TODO: Terrible kludge because widget system doesn't notify Waterfall that
    // it's being shown or hidden.
    if (is_active())
        toggle();
        //stop(false);
    waterfall.on_hide();
    View::on_hide();
}

void SigGenAppView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);

    const ui::Rect waterfall_rect{0, header_height, new_parent_rect.width(), new_parent_rect.height() - header_height};
    waterfall.set_parent_rect(waterfall_rect);
}

} /* namespace ui */
