#include "pod5_format/c_api.h"

#include "c_api_test_utils.h"
#include "pod5_format/file_reader.h"
#include "pod5_format/schema_metadata.h"
#include "pod5_format/uuid.h"
#include "pod5_format/version.h"
#include "utils.h"

#include <catch2/catch.hpp>
#include <gsl/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <vector>

struct Pod5ReadId {
    Pod5ReadId() = default;

    Pod5ReadId(pod5::Uuid const & uid) { uid.to_c_array(read_id); }

    pod5::Uuid as_uuid() const { return pod5::Uuid{read_id}; }

    bool operator==(Pod5ReadId const & other) const { return as_uuid() == other.as_uuid(); }

    read_id_t read_id;
};

std::ostream & operator<<(std::ostream & str, Pod5ReadId rid) { return str << rid.as_uuid(); }

SCENARIO("C API Reads", "[mytest1]")
{
    static constexpr char const * filename = "./foo_c_api.pod5";

    pod5_init();
    auto fin = gsl::finally([] { pod5_terminate(); });

    std::mt19937 gen{Catch::rngSeed()};
    auto uuid_gen = pod5::UuidRandomGenerator{gen};
    auto input_read_id = uuid_gen();
    auto input_read_id_2 = uuid_gen();
    std::vector<int16_t> signal_1(10);
    std::iota(signal_1.begin(), signal_1.end(), -20000);

    std::vector<int16_t> signal_2(20);
    std::iota(signal_2.begin(), signal_2.end(), 0);

    std::int16_t adc_min = -4096;
    std::int16_t adc_max = 4095;

    float calibration_offset = 54.0f;
    float calibration_scale = 100.0f;

    float predicted_scale = 2.3f;
    float predicted_shift = 10.0f;
    float tracked_scale = 4.3f;
    float tracked_shift = 15.0f;
    std::uint32_t num_reads_since_mux_change = 1234;
    float time_since_mux_change = 2.4f;
    std::uint64_t num_minknow_events = 104;

    // Write the file:
    {
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK_FALSE(pod5_create_file(NULL, "c_software", NULL));
        CHECK(pod5_get_error_no() == POD5_ERROR_INVALID);
        CHECK_FALSE(pod5_create_file("", "c_software", NULL));
        CHECK(pod5_get_error_no() == POD5_ERROR_INVALID);
        CHECK_FALSE(pod5_create_file("", NULL, NULL));
        CHECK(pod5_get_error_no() == POD5_ERROR_INVALID);

        REQUIRE(remove_file_if_exists(filename).ok());

        auto file = pod5_create_file(filename, "c_software", NULL);
        REQUIRE(file);
        CHECK_POD5_OK(pod5_get_error_no());

        std::int16_t pore_type_id = -1;
        CHECK_POD5_OK(pod5_add_pore(&pore_type_id, file, "pore_type"));
        CHECK(pore_type_id == 0);

        std::vector<char const *> context_tags_keys{"thing", "foo"};
        std::vector<char const *> context_tags_values{"thing_val", "foo_val"};
        std::vector<char const *> tracking_id_keys{"baz", "other"};
        std::vector<char const *> tracking_id_values{"baz_val", "other_val"};

        std::uint32_t read_number = 12;
        std::uint64_t start_sample = 10245;
        float median_before = 200.0f;
        std::uint16_t channel = 43;
        std::uint8_t well = 4;
        pod5_end_reason_t end_reason = POD5_END_REASON_MUX_CHANGE;
        uint8_t end_reason_forced = false;
        auto read_id_array = (read_id_t const *)input_read_id.data();

        std::int16_t run_info_id = 0;
        ReadBatchRowInfoArrayV3 row_data{
            read_id_array,
            &read_number,
            &start_sample,
            &median_before,
            &channel,
            &well,
            &pore_type_id,
            &calibration_offset,
            &calibration_scale,
            &end_reason,
            &end_reason_forced,
            &run_info_id,
            &num_minknow_events,
            &tracked_scale,
            &tracked_shift,
            &predicted_scale,
            &predicted_shift,
            &num_reads_since_mux_change,
            &time_since_mux_change};

        std::int16_t const * signal_arr[] = {signal_1.data()};
        std::uint32_t signal_size[] = {(std::uint32_t)signal_1.size()};

        // Referencing a non-existent run id should fail:
        CHECK(
            pod5_add_reads_data(
                file, 1, READ_BATCH_ROW_INFO_VERSION_3, &row_data, signal_arr, signal_size)
            == POD5_ERROR_INVALID);

        // Now actually add the run info:
        CHECK_POD5_OK(pod5_add_run_info(
            &run_info_id,
            file,
            "acquisition_id",
            15400,
            adc_max,
            adc_min,
            context_tags_keys.size(),
            context_tags_keys.data(),
            context_tags_values.data(),
            "experiment_name",
            "flow_cell_id",
            "flow_cell_product_code",
            "protocol_name",
            "protocol_run_id",
            200000,
            "sample_id",
            4000,
            "sequencing_kit",
            "sequencer_position",
            "sequencer_position_type",
            "software",
            "system_name",
            "system_type",
            tracking_id_keys.size(),
            tracking_id_keys.data(),
            tracking_id_values.data()));
        CHECK(run_info_id == 0);

        {
            CHECK_POD5_OK(pod5_add_reads_data(
                file, 1, READ_BATCH_ROW_INFO_VERSION_3, &row_data, signal_arr, signal_size));
        }

        {
            auto compressed_read_max_size = pod5_vbz_compressed_signal_max_size(signal_2.size());
            std::vector<char> compressed_signal(compressed_read_max_size);
            char const * compressed_data[] = {compressed_signal.data()};
            char const ** compressed_data_ptr = compressed_data;
            std::size_t compressed_size[] = {compressed_signal.size()};
            std::size_t const * compressed_size_ptr = compressed_size;
            std::uint32_t signal_size[] = {(std::uint32_t)signal_2.size()};
            std::uint32_t const * signal_size_ptr = signal_size;
            pod5_vbz_compress_signal(
                signal_2.data(), signal_2.size(), compressed_signal.data(), compressed_size);

            std::size_t signal_counts = 1;

            auto read_id_array = (read_id_t const *)input_read_id_2.data();
            row_data.read_id = read_id_array;

            CHECK_POD5_OK(pod5_add_reads_data_pre_compressed(
                file,
                1,
                READ_BATCH_ROW_INFO_VERSION_3,
                &row_data,
                &compressed_data_ptr,
                &compressed_size_ptr,
                &signal_size_ptr,
                &signal_counts));
        }

        CHECK_POD5_OK(pod5_close_and_free_writer(file));
        CHECK_POD5_OK(pod5_get_error_no());
    }

    // Read the file back:
    {
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK_FALSE(pod5_open_file(NULL));
        auto file = pod5_open_file(filename);
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK(file);

        FileInfo_t file_info;
        CHECK_POD5_OK(pod5_get_file_info(file, &file_info));
        CHECK(file_info.version.major == pod5::Pod5MajorVersion);
        CHECK(file_info.version.minor == pod5::Pod5MinorVersion);
        CHECK(file_info.version.revision == pod5::Pod5RevVersion);
        {
            auto reader = pod5::open_file_reader(filename);
            pod5::Uuid file_identifier{file_info.file_identifier};
            CHECK(file_identifier == (*reader)->schema_metadata().file_identifier);
        }

        std::size_t read_count = 0;
        CHECK_POD5_OK(pod5_get_read_count(file, &read_count));
        REQUIRE(read_count == 2);

        std::vector<Pod5ReadId> read_ids(2);
        CHECK(pod5_get_read_ids(file, 1, (read_id_t *)read_ids.data()) != POD5_OK);
        CHECK_POD5_OK(pod5_get_read_ids(file, read_ids.size(), (read_id_t *)read_ids.data()));
        std::vector<Pod5ReadId> expected_read_ids{input_read_id, input_read_id_2};
        CHECK(read_ids == expected_read_ids);

        std::size_t batch_count = 0;
        CHECK_POD5_OK(pod5_get_read_batch_count(&batch_count, file));
        REQUIRE(batch_count == 1);

        Pod5ReadRecordBatch * batch_0 = nullptr;
        CHECK_POD5_OK(pod5_get_read_batch(&batch_0, file, 0));
        REQUIRE(batch_0);

        std::size_t row_count = 0;
        CHECK_POD5_OK(pod5_get_read_batch_row_count(&row_count, batch_0));
        REQUIRE(row_count == 2);

        // Check out of bounds accesses get errors
        {
            ReadBatchRowInfoV3 v3_struct;
            uint16_t input_version = 0;
            CHECK(
                pod5_get_read_batch_row_info_data(
                    batch_0, row_count, READ_BATCH_ROW_INFO_VERSION, &v3_struct, &input_version)
                == POD5_ERROR_INDEXERROR);

            std::vector<uint64_t> signal_row_indices{1};
            CHECK(
                pod5_get_signal_row_indices(
                    batch_0, row_count, signal_row_indices.size(), signal_row_indices.data())
                == POD5_ERROR_INDEXERROR);

            CalibrationExtraData calibration_extra_data{};
            CHECK(
                pod5_get_calibration_extra_info(batch_0, row_count, &calibration_extra_data)
                == POD5_ERROR_INDEXERROR);
        }

        for (std::size_t row = 0; row < row_count; ++row) {
            auto signal = signal_1;
            if (row == 1) {
                signal = signal_2;
            }

            static_assert(
                std::is_same<ReadBatchRowInfoV3, ReadBatchRowInfo_t>::value,
                "Update this if new structs added");

            ReadBatchRowInfoV3 v3_struct;
            uint16_t input_version = 0;
            CHECK_POD5_OK(pod5_get_read_batch_row_info_data(
                batch_0, row, READ_BATCH_ROW_INFO_VERSION, &v3_struct, &input_version));
            CHECK(input_version == 3);

            std::string formatted_uuid(36, '\0');
            CHECK_POD5_OK(pod5_format_read_id(v3_struct.read_id, &formatted_uuid[0]));
            CHECK(
                formatted_uuid
                == to_string(*reinterpret_cast<pod5::Uuid const *>(v3_struct.read_id)));

            CHECK(v3_struct.read_number == 12);
            CHECK(v3_struct.start_sample == 10245);
            CHECK(v3_struct.median_before == 200.0f);
            CHECK(v3_struct.channel == 43);
            CHECK(v3_struct.well == 4);
            CHECK(v3_struct.pore_type == 0);
            CHECK(v3_struct.calibration_offset == calibration_offset);
            CHECK(v3_struct.calibration_scale == calibration_scale);
            CHECK(v3_struct.end_reason == 1);
            CHECK(v3_struct.end_reason_forced == uint8_t{false});
            CHECK(v3_struct.run_info == 0);
            CHECK(v3_struct.num_minknow_events == num_minknow_events);
            CHECK(v3_struct.tracked_scaling_scale == tracked_scale);
            CHECK(v3_struct.tracked_scaling_shift == tracked_shift);
            CHECK(v3_struct.predicted_scaling_scale == predicted_scale);
            CHECK(v3_struct.predicted_scaling_shift == predicted_shift);
            CHECK(v3_struct.num_reads_since_mux_change == num_reads_since_mux_change);
            CHECK(v3_struct.time_since_mux_change == time_since_mux_change);
            CHECK(v3_struct.signal_row_count == 1);
            CHECK(v3_struct.num_samples == signal.size());

            std::vector<uint64_t> signal_row_indices(v3_struct.signal_row_count);
            CHECK_POD5_OK(pod5_get_signal_row_indices(
                batch_0, row, signal_row_indices.size(), signal_row_indices.data()));

            std::vector<SignalRowInfo *> signal_row_info(v3_struct.signal_row_count);
            CHECK_POD5_OK(pod5_get_signal_row_info(
                file,
                signal_row_indices.size(),
                signal_row_indices.data(),
                signal_row_info.data()));

            std::vector<int16_t> read_signal(signal_row_info.front()->stored_sample_count);
            REQUIRE(signal_row_info.front()->stored_sample_count == signal.size());
            CHECK_POD5_OK(pod5_get_signal(
                file,
                signal_row_info.front(),
                signal_row_info.front()->stored_sample_count,
                read_signal.data()));
            CHECK(read_signal == signal);

            std::size_t sample_count = 0;
            CHECK_POD5_OK(pod5_get_read_complete_sample_count(file, batch_0, row, &sample_count));
            CHECK(sample_count == signal_row_info.front()->stored_sample_count);
            CHECK_POD5_OK(pod5_get_read_complete_signal(
                file, batch_0, row, sample_count, read_signal.data()));
            CHECK(read_signal == signal);

            CHECK_POD5_OK(
                pod5_free_signal_row_info(signal_row_indices.size(), signal_row_info.data()));

            std::string expected_pore_type{"pore_type"};
            std::array<char, 128> char_buffer{};
            std::size_t returned_size = 2;  // deliberately too short!
            {
                CHECK(
                    pod5_get_pore_type(
                        batch_0, v3_struct.pore_type, char_buffer.data(), &returned_size)
                    == POD5_ERROR_STRING_NOT_LONG_ENOUGH);
                CHECK(returned_size == expected_pore_type.size() + 1);
            }
            {
                returned_size = char_buffer.size();
                CHECK_POD5_OK(pod5_get_pore_type(
                    batch_0, v3_struct.pore_type, char_buffer.data(), &returned_size));
                CHECK(returned_size == expected_pore_type.size() + 1);
                CHECK(std::string{char_buffer.data()} == expected_pore_type);
            }
            {
                returned_size = char_buffer.size();
                CHECK(
                    pod5_get_pore_type(batch_0, -1, char_buffer.data(), &returned_size)
                    == POD5_ERROR_INDEXERROR);
                CHECK(returned_size == char_buffer.size());
            }

            std::string expected_end_reason{"mux_change"};
            {
                returned_size = 2;  // deliberately too short!
                pod5_end_reason end_reason = POD5_END_REASON_UNKNOWN;
                CHECK(
                    pod5_get_end_reason(
                        batch_0,
                        v3_struct.end_reason,
                        &end_reason,
                        char_buffer.data(),
                        &returned_size)
                    == POD5_ERROR_STRING_NOT_LONG_ENOUGH);
                CHECK(returned_size == expected_end_reason.size() + 1);
            }
            {
                returned_size = char_buffer.size();
                pod5_end_reason end_reason = POD5_END_REASON_UNKNOWN;
                CHECK_POD5_OK(pod5_get_end_reason(
                    batch_0,
                    v3_struct.end_reason,
                    &end_reason,
                    char_buffer.data(),
                    &returned_size));
                CHECK(returned_size == expected_end_reason.size() + 1);
                CHECK(end_reason == POD5_END_REASON_MUX_CHANGE);
                CHECK(std::string{char_buffer.data()} == expected_end_reason);
            }
            // Check getting with an invalid input end reason index:
            {
                returned_size = char_buffer.size();
                pod5_end_reason end_reason = POD5_END_REASON_UNKNOWN;
                CHECK(
                    pod5_get_end_reason(
                        batch_0,
                        v3_struct.end_reason + 100,
                        &end_reason,
                        char_buffer.data(),
                        &returned_size)
                    == POD5_ERROR_INDEXERROR);
                CHECK(returned_size == char_buffer.size());
                CHECK(end_reason == POD5_END_REASON_UNKNOWN);
            }

            CalibrationExtraData calibration_extra_data{};
            CHECK_POD5_OK(pod5_get_calibration_extra_info(batch_0, row, &calibration_extra_data));
            CHECK(calibration_extra_data.digitisation == adc_max - adc_min + 1);
            CHECK(calibration_extra_data.range == 8192 * calibration_scale);
        }

        SECTION("Embedded files")
        {
            for (auto [get_file_location, name] : {
                     std::tuple(
                         pod5_get_file_read_table_location, "pod5_get_file_read_table_location"),
                     std::tuple(
                         pod5_get_file_signal_table_location,
                         "pod5_get_file_signal_table_location"),
                     std::tuple(
                         pod5_get_file_run_info_table_location,
                         "pod5_get_file_run_info_table_location"),
                 })
            {
                CAPTURE(name);
                EmbeddedFileData_t embedded_file_data{};
                CHECK_POD5_OK(get_file_location(file, &embedded_file_data));
                REQUIRE(embedded_file_data.file_name != nullptr);
                CHECK(embedded_file_data.file_name == std::string_view{filename});
                CHECK(embedded_file_data.offset > 0);
                CHECK(embedded_file_data.length > 0);
            }
        }

        run_info_index_t run_info_count = 0;
        CHECK_POD5_OK(pod5_get_file_run_info_count(file, &run_info_count));
        REQUIRE(run_info_count == 1);

        // Check getting invalid run info indexes fails correctly.
        RunInfoDictData * run_info_error = nullptr;
        CHECK(pod5_get_run_info(batch_0, -1, &run_info_error) == POD5_ERROR_INDEXERROR);
        CHECK_FALSE(run_info_error);
        CHECK(pod5_get_run_info(batch_0, run_info_count, &run_info_error) == POD5_ERROR_INDEXERROR);
        CHECK_FALSE(run_info_error);
        CHECK(pod5_get_file_run_info(file, -1, &run_info_error) == POD5_ERROR_INDEXERROR);
        CHECK_FALSE(run_info_error);
        CHECK(
            pod5_get_file_run_info(file, run_info_count, &run_info_error) == POD5_ERROR_INDEXERROR);
        CHECK_FALSE(run_info_error);

        auto check_run_info = [](RunInfoDictData * run_info) {
            REQUIRE(run_info);
            CHECK(run_info->tracking_id.size == 2);
            CHECK(run_info->tracking_id.keys[0] == std::string("baz"));
            CHECK(run_info->tracking_id.keys[1] == std::string("other"));
            CHECK(run_info->tracking_id.values[0] == std::string("baz_val"));
            CHECK(run_info->tracking_id.values[1] == std::string("other_val"));
            CHECK(run_info->context_tags.size == 2);
            CHECK(run_info->context_tags.keys[0] == std::string("thing"));
            CHECK(run_info->context_tags.keys[1] == std::string("foo"));
            CHECK(run_info->context_tags.values[0] == std::string("thing_val"));
            CHECK(run_info->context_tags.values[1] == std::string("foo_val"));
        };

        RunInfoDictData * run_info_data_out_1 = nullptr;
        CHECK_POD5_OK(pod5_get_file_run_info(file, 0, &run_info_data_out_1));
        check_run_info(run_info_data_out_1);
        pod5_free_run_info(run_info_data_out_1);

        RunInfoDictData * run_info_data_out_2 = nullptr;
        CHECK_POD5_OK(pod5_get_run_info(batch_0, 0, &run_info_data_out_2));
        check_run_info(run_info_data_out_2);
        pod5_free_run_info(run_info_data_out_2);

        pod5_free_read_batch(batch_0);

        pod5_close_and_free_reader(file);
        CHECK_POD5_OK(pod5_get_error_no());
    }
}

SCENARIO("C API Many Reads")
{
    static constexpr char const * filename = "./foo_c_api.pod5";

    pod5_init();
    auto fin = gsl::finally([] { pod5_terminate(); });

    std::mt19937 gen{Catch::rngSeed()};
    auto uuid_gen = pod5::UuidRandomGenerator{gen};
    std::vector<int16_t> signal_1(10);
    std::iota(signal_1.begin(), signal_1.end(), -20000);

    std::vector<int16_t> signal_2(20);
    std::iota(signal_2.begin(), signal_2.end(), 0);

    std::size_t const read_count = 10037;

    std::int16_t const adc_min = -4096;
    std::int16_t const adc_max = 4095;

    std::vector<pod5::Uuid> read_id_array(read_count);
    std::generate(read_id_array.begin(), read_id_array.end(), uuid_gen);

    // Write the file:
    {
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK_FALSE(pod5_create_file(NULL, "c_software", NULL));
        CHECK(pod5_get_error_no() == POD5_ERROR_INVALID);
        CHECK_FALSE(pod5_create_file("", "c_software", NULL));
        CHECK(pod5_get_error_no() == POD5_ERROR_INVALID);
        CHECK_FALSE(pod5_create_file("", NULL, NULL));
        CHECK(pod5_get_error_no() == POD5_ERROR_INVALID);

        REQUIRE(remove_file_if_exists(filename).ok());

        auto file = pod5_create_file(filename, "c_software", NULL);
        REQUIRE(file);
        CHECK_POD5_OK(pod5_get_error_no());

        std::int16_t pore_type_id = -1;
        CHECK_POD5_OK(pod5_add_pore(&pore_type_id, file, "pore_type"));
        CHECK(pore_type_id == 0);

        std::vector<char const *> context_tags_keys{"thing", "foo"};
        std::vector<char const *> context_tags_values{"thing_val", "foo_val"};
        std::vector<char const *> tracking_id_keys{"baz", "other"};
        std::vector<char const *> tracking_id_values{"baz_val", "other_val"};

        std::int16_t run_info_id = -1;
        CHECK_POD5_OK(pod5_add_run_info(
            &run_info_id,
            file,
            "acquisition_id",
            15400,
            adc_max,
            adc_min,
            context_tags_keys.size(),
            context_tags_keys.data(),
            context_tags_values.data(),
            "experiment_name",
            "flow_cell_id",
            "flow_cell_product_code",
            "protocol_name",
            "protocol_run_id",
            200000,
            "sample_id",
            4000,
            "sequencing_kit",
            "sequencer_position",
            "sequencer_position_type",
            "software",
            "system_name",
            "system_type",
            tracking_id_keys.size(),
            tracking_id_keys.data(),
            tracking_id_values.data()));
        CHECK(run_info_id == 0);

        std::vector<std::uint32_t> read_number(read_count, 12);
        std::vector<std::uint64_t> start_sample(read_count, 10245);
        std::vector<float> median_before(read_count, 200.0f);
        std::vector<std::uint16_t> channel(read_count, 43);
        std::vector<std::uint8_t> well(read_count, 4);
        std::vector<pod5_end_reason_t> end_reason(read_count, POD5_END_REASON_MUX_CHANGE);
        std::vector<uint8_t> end_reason_forced(read_count, false);

        std::vector<float> calibration_offset(read_count, 54.0f);
        std::vector<float> calibration_scale(read_count, 100.0f);

        std::vector<float> predicted_scale(read_count, 2.3f);
        std::vector<float> predicted_shift(read_count, 10.0f);
        std::vector<float> tracked_scale(read_count, 4.3f);
        std::vector<float> tracked_shift(read_count, 15.0f);
        std::vector<std::uint32_t> num_reads_since_mux_change(read_count, 1234);
        std::vector<float> time_since_mux_change(read_count, 2.4f);
        std::vector<std::uint64_t> num_minknow_events(read_count, 104);

        std::vector<std::int16_t> pore_type_ids(read_count, pore_type_id);
        std::vector<std::int16_t> run_info_ids(read_count, run_info_id);

        std::vector<std::int16_t const *> signal_arr;
        std::vector<std::uint32_t> signal_size;
        ReadBatchRowInfoArrayV3 row_data{
            (read_id_t *)read_id_array.data(),
            read_number.data(),
            start_sample.data(),
            median_before.data(),
            channel.data(),
            well.data(),
            pore_type_ids.data(),
            calibration_offset.data(),
            calibration_scale.data(),
            end_reason.data(),
            end_reason_forced.data(),
            run_info_ids.data(),
            num_minknow_events.data(),
            tracked_scale.data(),
            tracked_shift.data(),
            predicted_scale.data(),
            predicted_shift.data(),
            num_reads_since_mux_change.data(),
            time_since_mux_change.data()};

        for (std::size_t i = 0; i < read_count; ++i) {
            signal_arr.push_back(signal_1.data());
            signal_size.push_back((std::uint32_t)signal_1.size());
        }

        CHECK_POD5_OK(pod5_add_reads_data(
            file,
            read_count,
            READ_BATCH_ROW_INFO_VERSION_3,
            &row_data,
            signal_arr.data(),
            signal_size.data()));

        CHECK_POD5_OK(pod5_close_and_free_writer(file));
        CHECK_POD5_OK(pod5_get_error_no());
    }

    // Read the file back:
    {
        Pod5ReaderOptions_t options{};
        options.force_disable_file_mapping = true;

        CHECK_POD5_OK(pod5_get_error_no());
        CHECK_FALSE(pod5_open_file_options(NULL, &options));
        CHECK_FALSE(pod5_open_file_options(filename, NULL));
        auto file = pod5_open_file_options(filename, &options);
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK(file);

        FileInfo_t file_info;
        CHECK_POD5_OK(pod5_get_file_info(file, &file_info));
        CHECK(file_info.version.major == pod5::Pod5MajorVersion);
        CHECK(file_info.version.minor == pod5::Pod5MinorVersion);
        CHECK(file_info.version.revision == pod5::Pod5RevVersion);
        {
            auto reader = pod5::open_file_reader(filename);
            pod5::Uuid file_identifier{file_info.file_identifier};
            CHECK(file_identifier == (*reader)->schema_metadata().file_identifier);
        }

        std::size_t read_count_returned = 0;
        CHECK_POD5_OK(pod5_get_read_count(file, &read_count_returned));
        REQUIRE(read_count_returned == read_count);

        // Randomise the order of the read IDs and then try and plan a path through them.
        std::shuffle(read_id_array.begin(), read_id_array.end(), gen);
        std::vector<std::uint32_t> batch_counts(read_count);
        std::vector<std::uint32_t> batch_rows(read_count);
        std::size_t find_success_count = 0;
        CHECK_POD5_OK(pod5_plan_traversal(
            file,
            reinterpret_cast<uint8_t const *>(read_id_array.data()),
            read_count,
            batch_counts.data(),
            batch_rows.data(),
            &find_success_count));
        REQUIRE(find_success_count == read_count);

        CHECK_POD5_OK(pod5_close_and_free_reader(file));
    }
}

SCENARIO("C API Run Info")
{
    static constexpr char const * filename = "./foo_c_api.pod5";

    pod5_init();
    auto fin = gsl::finally([] { pod5_terminate(); });

    std::int16_t adc_min = -4096;
    std::int16_t adc_max = 4095;

    auto expected_acq_id = [](std::size_t index) {
        std::string acquisition_id{"acquisition_id_"};
        acquisition_id += std::to_string(index);
        return acquisition_id;
    };

    // Write the file:
    {
        REQUIRE(remove_file_if_exists(filename).ok());

        auto file = pod5_create_file(filename, "c_software", NULL);
        REQUIRE(file);
        CHECK_POD5_OK(pod5_get_error_no());

        std::vector<char const *> context_tags_keys{"thing", "foo"};
        std::vector<char const *> context_tags_values{"thing_val", "foo_val"};
        std::vector<char const *> tracking_id_keys{"baz", "other"};
        std::vector<char const *> tracking_id_values{"baz_val", "other_val"};

        for (std::size_t i = 0; i < 10; ++i) {
            std::int16_t run_info_id = -1;
            CHECK_POD5_OK(pod5_add_run_info(
                &run_info_id,
                file,
                expected_acq_id(i).c_str(),
                15400,
                adc_max,
                adc_min,
                context_tags_keys.size(),
                context_tags_keys.data(),
                context_tags_values.data(),
                "experiment_name",
                "flow_cell_id",
                "flow_cell_product_code",
                "protocol_name",
                "protocol_run_id",
                200000,
                "sample_id",
                4000,
                "sequencing_kit",
                "sequencer_position",
                "sequencer_position_type",
                "software",
                "system_name",
                "system_type",
                tracking_id_keys.size(),
                tracking_id_keys.data(),
                tracking_id_values.data()));
            CHECK(run_info_id == static_cast<std::int16_t>(i));
        }
        CHECK_POD5_OK(pod5_close_and_free_writer(file));
    }

    // Read the file back:
    {
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK_FALSE(pod5_open_file(NULL));
        auto file = pod5_open_file(filename);
        CHECK_POD5_OK(pod5_get_error_no());
        CHECK(pod5_get_error_string() == std::string{""});
        REQUIRE(file);

        run_info_index_t run_info_count = 0;
        CHECK_POD5_OK(pod5_get_file_run_info_count(file, &run_info_count));
        REQUIRE(run_info_count == 10);

        for (run_info_index_t i = 0; i < 10; ++i) {
            RunInfoDictData * run_info_data_out = nullptr;
            CHECK_POD5_OK(pod5_get_file_run_info(file, i, &run_info_data_out));
            CHECK(run_info_data_out->acquisition_id == expected_acq_id(i));
            pod5_free_run_info(run_info_data_out);
        }

        CHECK_POD5_OK(pod5_close_and_free_reader(file));
    }
}

TEST_CASE("Missing file passed to pod5_open_file")
{
    pod5_init();
    auto cleanup = gsl::finally([] { pod5_terminate(); });

    static constexpr char const temporary_filename[] = "./foo_c_api.pod5";
    REQUIRE(remove_file_if_exists(temporary_filename).ok());

    CHECK(pod5_open_file(temporary_filename) == nullptr);
}

TEST_CASE("Existing file passed to pod5_create_file")
{
    pod5_init();
    auto cleanup = gsl::finally([] { pod5_terminate(); });

    static constexpr char const temporary_filename[] = "./foo_c_api.pod5";
    REQUIRE(remove_file_if_exists(temporary_filename).ok());

    // Create it once.
    Pod5FileWriter_t * writer = pod5_create_file(temporary_filename, "c_software", nullptr);
    REQUIRE_POD5_OK(pod5_get_error_no());
    REQUIRE(writer != nullptr);
    REQUIRE_POD5_OK(pod5_close_and_free_writer(writer));

    // File already exists so this should fail.
    CHECK(pod5_create_file(temporary_filename, "c_software", nullptr) == nullptr);
}

TEST_CASE("pod5_create_file with options")
{
    pod5_init();
    auto cleanup = gsl::finally([] { pod5_terminate(); });

    static constexpr char const temporary_filename[] = "./foo_c_api.pod5";
    REQUIRE(remove_file_if_exists(temporary_filename).ok());

    Pod5WriterOptions_t test_options{};
    Pod5WriterOptions_t const * options = nullptr;
    bool const with_options = GENERATE(false, true);
    if (with_options) {
        options = &test_options;
    } else {
        test_options.max_signal_chunk_size = GENERATE(0, 1, 2);
        test_options.signal_compression_type = GENERATE(
            CompressionOption::DEFAULT_SIGNAL_COMPRESSION,
            CompressionOption::VBZ_SIGNAL_COMPRESSION,
            CompressionOption::UNCOMPRESSED_SIGNAL);
        test_options.signal_table_batch_size = GENERATE(0, 1, 2);
        test_options.read_table_batch_size = GENERATE(0, 1, 2);
    }

    CAPTURE(
        with_options,
        test_options.max_signal_chunk_size,
        test_options.signal_compression_type,
        test_options.signal_table_batch_size,
        test_options.read_table_batch_size);

    Pod5FileWriter_t * writer = pod5_create_file(temporary_filename, "c_software", options);
    REQUIRE_POD5_OK(pod5_get_error_no());
    REQUIRE(writer != nullptr);
    REQUIRE_POD5_OK(pod5_close_and_free_writer(writer));
}

TEST_CASE("VBZ compression", "[mytest2]")
{
    pod5_init();
    auto cleanup = gsl::finally([] { pod5_terminate(); });

    std::size_t const sample_count = 20;
    std::vector<int16_t> input_signal(sample_count);
    std::iota(input_signal.begin(), input_signal.end(), -sample_count / 2);

    // Determine max size.
    std::size_t const compressed_read_max_size =
        pod5_vbz_compressed_signal_max_size(input_signal.size());
    REQUIRE(compressed_read_max_size > 0);

    // Compress it.
    std::vector<char> compressed_signal(compressed_read_max_size);
    std::size_t compressed_size = compressed_read_max_size;
    REQUIRE_POD5_OK(pod5_vbz_compress_signal(
        input_signal.data(), input_signal.size(), compressed_signal.data(), &compressed_size));
    REQUIRE(compressed_size <= compressed_read_max_size);
    compressed_signal.resize(compressed_size);
    std::cout << "Original size: " << input_signal.size() << std::endl;
    std::cout << "Compressed size: " << compressed_signal.size() << std::endl;
    std::cout << "Compression ratio: " << 1.0f * input_signal.size() / compressed_signal.size() << std::endl;

    // Decompress it.
    std::vector<int16_t> output_signal(sample_count);
    REQUIRE_POD5_OK(pod5_vbz_decompress_signal(
        compressed_signal.data(), compressed_signal.size(), sample_count, output_signal.data()));
    REQUIRE(input_signal == output_signal);

    // Providing incorrect buffer sizes should fail rather than crash.
    CHECK_POD5_NOT_OK(pod5_vbz_decompress_signal(
        compressed_signal.data(),
        compressed_signal.size(),
        sample_count * 2,
        output_signal.data()));
    std::size_t bad_compressed_size = compressed_size / 2;
    CHECK_POD5_NOT_OK(pod5_vbz_compress_signal(
        input_signal.data(), input_signal.size(), compressed_signal.data(), &bad_compressed_size));

    // Going over the maximum size should produce an error.
    size_t const max_size_error = pod5_vbz_compressed_signal_max_size(std::uint64_t{1} << 48);
    CHECK(max_size_error == 0);
    CHECK_POD5_NOT_OK(pod5_get_error_no());
}

TEST_CASE("Read binary signal and save to POD5 with compression stats", "[mytest3]")
{
    // 1. 初始化POD5环境
    pod5_init();
    auto cleanup = gsl::finally([] { pod5_terminate(); });

    // 2. 定义文件路径
    // const std::string input_binary_file = "/ssdData/reads_test_dat/reads_10.dat";
    // const std::string input_binary_file = "/ssdData/reads_test_dat/reads_20.dat";
    // const std::string input_binary_file = "/ssdData/reads_test_dat/reads_30.dat";
    const std::string input_binary_file = "../../../test_data/int16_export/FAY22732_pass_barcode81_6af3f71b_1accfdb0_0.dat";
    const std::string output_pod5_file = "../../../test_data/int16_export/FAY22732_pass_barcode81_6af3f71b_1accfdb0_0.pod5";

    // 3. 从二进制文件读取信号数据
    std::vector<int16_t> signal_data;
    {
        std::ifstream ifs(input_binary_file, std::ios::binary | std::ios::ate);
        REQUIRE(ifs.is_open());

        auto file_size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);

        REQUIRE(file_size % sizeof(int16_t) == 0);
        signal_data.resize(file_size / sizeof(int16_t));
        ifs.read(reinterpret_cast<char*>(signal_data.data()), file_size);
    }

    std::cout << "Read " << signal_data.size() << " samples from binary file\n";

    // 4. 创建POD5文件并写入数据
    {
        // 删除已存在的文件
        REQUIRE(remove_file_if_exists(output_pod5_file).ok());

        // 创建POD5文件
        auto file = pod5_create_file(output_pod5_file.c_str(), "signal_writer", nullptr);
        REQUIRE(file);
        CHECK_POD5_OK(pod5_get_error_no());

        // 添加pore类型
        std::int16_t pore_type_id = -1;
        CHECK_POD5_OK(pod5_add_pore(&pore_type_id, file, "test_pore"));
        REQUIRE(pore_type_id == 0);

        // 添加run信息
        std::vector<char const *> context_tags_keys{"source", "test"};
        std::vector<char const *> context_tags_values{"binary_file", "compression_test"};
        std::vector<char const *> tracking_id_keys{"device", "operator"};
        std::vector<char const *> tracking_id_values{"test_device", "tester"};

        std::int16_t run_info_id = -1;
        CHECK_POD5_OK(pod5_add_run_info(
            &run_info_id,
            file,
            "binary_signal_acq",
            15400,
            4095,   // adc_max
            -4096,  // adc_min
            context_tags_keys.size(),
            context_tags_keys.data(),
            context_tags_values.data(),
            "binary_signal_test",
            "test_flowcell",
            "TEST001",
            "test_protocol",
            "test_run_001",
            200000,
            "test_sample",
            4000,
            "test_kit",
            "position_A1",
            "test_position",
            "test_software",
            "test_system",
            "test_type",
            tracking_id_keys.size(),
            tracking_id_keys.data(),
            tracking_id_values.data()));
        REQUIRE(run_info_id == 0);

        // 准备读取数据
        std::mt19937 gen{Catch::rngSeed()};
        auto uuid_gen = pod5::UuidRandomGenerator{gen};
        auto read_id = uuid_gen();

        std::uint32_t read_number = 1;
        std::uint64_t start_sample = 0;
        float median_before = 0.0f;
        std::uint16_t channel = 1;
        std::uint8_t well = 1;
        pod5_end_reason_t end_reason = POD5_END_REASON_UNKNOWN;
        uint8_t end_reason_forced = false;
        float calibration_offset = 0.0f;
        float calibration_scale = 1.0f;
        float predicted_scale = 1.0f;
        float predicted_shift = 0.0f;
        float tracked_scale = 1.0f;
        float tracked_shift = 0.0f;
        std::uint32_t num_reads_since_mux_change = 0;
        float time_since_mux_change = 0.0f;
        std::uint64_t num_minknow_events = 0;

        ReadBatchRowInfoArrayV3 row_data{
            (read_id_t const *)read_id.data(),
            &read_number,
            &start_sample,
            &median_before,
            &channel,
            &well,
            &pore_type_id,
            &calibration_offset,
            &calibration_scale,
            &end_reason,
            &end_reason_forced,
            &run_info_id,
            &num_minknow_events,
            &tracked_scale,
            &tracked_shift,
            &predicted_scale,
            &predicted_shift,
            &num_reads_since_mux_change,
            &time_since_mux_change};

        // 计算压缩比
        std::size_t original_size = signal_data.size() * sizeof(int16_t);
        
        // 压缩信号
        auto compressed_max_size = pod5_vbz_compressed_signal_max_size(signal_data.size());
        std::vector<char> compressed_signal(compressed_max_size);
        std::size_t compressed_size = compressed_max_size;

        auto start           = std::chrono::high_resolution_clock::now();
        CHECK_POD5_OK(pod5_vbz_compress_signal(
            signal_data.data(), signal_data.size(), compressed_signal.data(), &compressed_size));
        auto end             = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // 打印压缩信息
        std::cout << "\nCompression stats:\n";
        std::cout << "Original size: " << original_size << " bytes\n";
        std::cout << "Compressed size: " << compressed_size << " bytes\n";
        std::cout << "Compression time: " << duration << " ms\n";
        std::cout << "Compression ratio: " 
                 << static_cast<float>(original_size) / compressed_size << "\n";
        std::cout << "Space savings: " 
                 << 100.0f * (1.0f - static_cast<float>(compressed_size) / original_size) 
                 << "%\n";

        // 写入压缩后的数据
        char const * compressed_data[] = {compressed_signal.data()};
        char const ** compressed_data_ptr = compressed_data;
        std::size_t compressed_size_arr[] = {compressed_size};
        std::size_t const * compressed_size_ptr = compressed_size_arr;
        std::uint32_t signal_size_arr[] = {(std::uint32_t)signal_data.size()};
        std::uint32_t const * signal_size_ptr = signal_size_arr;
        std::size_t signal_counts = 1;

        CHECK_POD5_OK(pod5_add_reads_data_pre_compressed(
            file,
            1,
            READ_BATCH_ROW_INFO_VERSION_3,
            &row_data,
            &compressed_data_ptr,
            &compressed_size_ptr,
            &signal_size_ptr,
            &signal_counts));

        // 关闭文件
        CHECK_POD5_OK(pod5_close_and_free_writer(file));
    }

    // 5. 验证写入的数据
    {
        auto file = pod5_open_file(output_pod5_file.c_str());
        REQUIRE(file);

        std::size_t read_count = 0;
        CHECK_POD5_OK(pod5_get_read_count(file, &read_count));
        REQUIRE(read_count == 1);

        pod5::Uuid read_id_out;
        CHECK_POD5_OK(pod5_get_read_ids(file, 1, (read_id_t *)read_id_out.data()));

        Pod5ReadRecordBatch * batch = nullptr;
        CHECK_POD5_OK(pod5_get_read_batch(&batch, file, 0));
        REQUIRE(batch);

        ReadBatchRowInfoV3 row_info;
        uint16_t version;
        CHECK_POD5_OK(pod5_get_read_batch_row_info_data(
            batch, 0, READ_BATCH_ROW_INFO_VERSION, &row_info, &version));
        
        REQUIRE(row_info.num_samples == signal_data.size());

        std::vector<int16_t> read_back_signal(row_info.num_samples);
        CHECK_POD5_OK(pod5_get_read_complete_signal(
            file, batch, 0, row_info.num_samples, read_back_signal.data()));

        // 验证数据一致性
        REQUIRE(read_back_signal == signal_data);

        pod5_free_read_batch(batch);
        pod5_close_and_free_reader(file);
    }
}

namespace {

bool is_pod5_file(std::filesystem::path const & path)
{
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".pod5";
}

std::vector<std::filesystem::path> collect_pod5_inputs(std::filesystem::path const & input)
{
    REQUIRE(std::filesystem::exists(input));
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(input)) {
        for (auto const & entry : std::filesystem::directory_iterator(input)) {
            if (entry.is_regular_file() && is_pod5_file(entry.path())) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(input);
    }
    REQUIRE_FALSE(files.empty());
    return files;
}

struct Int16ExportSummary {
    std::string input_pod5;
    std::string output_dat;
    std::uint64_t pod5_bytes = 0;
    std::uint64_t int16_bytes = 0;
    std::size_t total_reads = 0;
    std::size_t exported = 0;
    double compression_ratio = 0.0;
    double space_savings = 0.0;
    bool full_export = false;
};

Int16ExportSummary export_one_pod5_to_int16(
    std::filesystem::path const & input_path,
    std::filesystem::path const & output_root,
    std::size_t max_reads,
    std::string const & sizes_filename,
    std::string const & stats_filename)
{
    Int16ExportSummary summary;
    summary.input_pod5 = input_path.string();

    auto reader = pod5_open_file(summary.input_pod5.c_str());
    REQUIRE(reader);
    REQUIRE_POD5_OK(pod5_get_error_no());

    std::size_t total_reads = 0;
    REQUIRE_POD5_OK(pod5_get_read_count(reader, &total_reads));
    REQUIRE(total_reads > 0);
    summary.total_reads = total_reads;

    std::size_t const export_limit =
        (max_reads == 0) ? total_reads : std::min(total_reads, max_reads);

    std::filesystem::create_directories(output_root);
    auto const output_dat =
        (output_root / input_path.stem()).string() + ".dat";
    summary.output_dat = output_dat;
    REQUIRE(remove_file_if_exists(output_dat).ok());

    std::ofstream dat_out(output_dat, std::ios::binary);
    REQUIRE(dat_out.is_open());

    auto const sizes_path = (output_root / sizes_filename).string();
    std::ofstream sizes_tsv(sizes_path);
    REQUIRE(sizes_tsv.is_open());
    sizes_tsv << "index\tread_id\tsamples\tuncompressed_bytes\n";

    std::vector<std::uint64_t> size_bytes;
    size_bytes.reserve(export_limit);

    std::size_t batch_count = 0;
    REQUIRE_POD5_OK(pod5_get_read_batch_count(&batch_count, reader));

    std::size_t exported = 0;
    auto const t0 = std::chrono::high_resolution_clock::now();

    for (std::size_t batch_idx = 0; batch_idx < batch_count && exported < export_limit;
         ++batch_idx)
    {
        Pod5ReadRecordBatch * batch = nullptr;
        REQUIRE_POD5_OK(pod5_get_read_batch(&batch, reader, batch_idx));
        REQUIRE(batch);

        std::size_t row_count = 0;
        REQUIRE_POD5_OK(pod5_get_read_batch_row_count(&row_count, batch));

        for (std::size_t row = 0; row < row_count && exported < export_limit; ++row, ++exported) {
            ReadBatchRowInfoV3 row_info{};
            uint16_t version = 0;
            REQUIRE_POD5_OK(pod5_get_read_batch_row_info_data(
                batch, row, READ_BATCH_ROW_INFO_VERSION, &row_info, &version));

            std::size_t sample_count = 0;
            REQUIRE_POD5_OK(
                pod5_get_read_complete_sample_count(reader, batch, row, &sample_count));
            REQUIRE(sample_count == row_info.num_samples);

            std::vector<int16_t> signal(sample_count);
            if (sample_count > 0) {
                REQUIRE_POD5_OK(pod5_get_read_complete_signal(
                    reader, batch, row, sample_count, signal.data()));
            }

            std::uint64_t const bytes = sample_count * sizeof(int16_t);
            size_bytes.push_back(bytes);
            if (bytes > 0) {
                dat_out.write(
                    reinterpret_cast<char const *>(signal.data()),
                    static_cast<std::streamsize>(bytes));
            }
            REQUIRE(dat_out.good());

            std::string read_id(36, '\0');
            REQUIRE_POD5_OK(pod5_format_read_id(row_info.read_id, read_id.data()));
            read_id.resize(36);
            sizes_tsv << exported << '\t' << read_id << '\t' << sample_count << '\t' << bytes
                      << '\n';
        }

        pod5_free_read_batch(batch);
    }

    dat_out.flush();
    sizes_tsv.flush();
    REQUIRE_POD5_OK(pod5_close_and_free_reader(reader));
    REQUIRE(exported == export_limit);
    REQUIRE(!size_bytes.empty());
    summary.exported = exported;

    auto sorted = size_bytes;
    std::sort(sorted.begin(), sorted.end());
    auto const n = sorted.size();
    auto const min_b = sorted.front();
    auto const max_b = sorted.back();
    double const sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    double const mean = sum / static_cast<double>(n);
    double acc = 0.0;
    for (auto b : sorted) {
        double const d = static_cast<double>(b) - mean;
        acc += d * d;
    }
    double const stddev = std::sqrt(acc / static_cast<double>(n));
    auto percentile = [&](double p) -> std::uint64_t {
        auto idx = static_cast<std::size_t>(std::round(p * static_cast<double>(n - 1)));
        return sorted[std::min(idx, n - 1)];
    };
    double const cv = (mean > 0.0) ? (stddev / mean) : 0.0;
    double const rel_range = (mean > 0.0) ? ((max_b - min_b) / mean) : 0.0;

    auto const t1 = std::chrono::high_resolution_clock::now();
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    auto const in_sz = std::filesystem::file_size(input_path);
    auto const out_sz = std::filesystem::file_size(output_dat);
    bool const full_export = (exported == total_reads);
    double const compression_ratio =
        (in_sz > 0) ? (static_cast<double>(out_sz) / static_cast<double>(in_sz)) : 0.0;
    double const space_savings = (out_sz > 0)
        ? (100.0 * (1.0 - static_cast<double>(in_sz) / static_cast<double>(out_sz)))
        : 0.0;

    summary.pod5_bytes = in_sz;
    summary.int16_bytes = out_sz;
    summary.compression_ratio = compression_ratio;
    summary.space_savings = space_savings;
    summary.full_export = full_export;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n[mytest4] Export POD5 -> same-named int16 .dat\n";
    std::cout << "Input:  " << summary.input_pod5 << "  (" << in_sz << " bytes)\n";
    std::cout << "Output: " << output_dat << "  (" << out_sz << " bytes)\n";
    std::cout << "Reads in file / exported: " << total_reads << " / " << exported << "\n";
    std::cout << "Elapsed: " << ms << " ms\n";
    std::cout << "Compression ratio (int16/POD5): " << compression_ratio;
    if (full_export) {
        std::cout << "  (original int16 / pod5)\n";
        std::cout << "Space savings: " << space_savings << " %\n";
    } else {
        std::cout << "  (exported int16 only; not comparable to full POD5)\n";
    }
    std::cout << "\nPer-read uncompressed int16 size stats (bytes):\n";
    std::cout << "  min    = " << min_b << "  (" << (min_b / 2) << " samples)\n";
    std::cout << "  p50    = " << percentile(0.50) << "\n";
    std::cout << "  p90    = " << percentile(0.90) << "\n";
    std::cout << "  p99    = " << percentile(0.99) << "\n";
    std::cout << "  max    = " << max_b << "  (" << (max_b / 2) << " samples)\n";
    std::cout << "  mean   = " << mean << "\n";
    std::cout << "  stddev = " << stddev << "\n";
    std::cout << "  CV (stddev/mean)     = " << cv << "   (越小越接近)\n";
    std::cout << "  (max-min)/mean       = " << rel_range << "\n";
    std::cout << "  total uncompressed   = " << sum << " bytes\n";

    auto const stats_path = (output_root / stats_filename).string();
    std::ofstream stats_txt(stats_path);
    REQUIRE(stats_txt.is_open());
    stats_txt << std::fixed << std::setprecision(6);
    stats_txt << "n=" << n << "\nmin=" << min_b << "\nmax=" << max_b << "\nmean=" << mean
              << "\nstddev=" << stddev << "\ncv=" << cv << "\nrel_range=" << rel_range
              << "\np50=" << percentile(0.50) << "\np90=" << percentile(0.90)
              << "\np99=" << percentile(0.99) << "\ninput_pod5_bytes=" << in_sz
              << "\noutput_dat_bytes=" << out_sz
              << "\ncompression_ratio_int16_over_pod5=" << compression_ratio
              << "\nspace_savings_percent=" << space_savings
              << "\nfull_export=" << (full_export ? 1 : 0) << "\n";

    std::cout << "\nSize similarity: CV=" << cv << ", rel_range=" << rel_range
              << ((cv < 0.10 && rel_range < 0.50) ? "  -> similar\n" : "  -> not similar\n");

    return summary;
}

}  // namespace

TEST_CASE("Export POD5 reads to a same-named int16 .dat", "[mytest4]")
{
    // 从已有 POD5 解出原始 int16，写成与源文件同名的 .dat（只改后缀）。
    // input 可以是单个 .pod5，也可以是含多个 .pod5 的文件夹（只扫描一层，不递归）。
    pod5_init();
    auto cleanup = gsl::finally([] { pod5_terminate(); });

    const std::string input_pod5 =
        "../../../test_data/AMtb_1__202402/FAY22732_pass_barcode81_6af3f71b_1accfdb0_0.pod5";
    // const std::string input_pod5 = "../../../test_data/AMtb_1__202402";
    // const std::string input_pod5 = "../../../test_data/Klebsiella_pneumoniae_KPC2";
    const std::string output_root_base = "../../../test_data/int16_export";
    // 0 表示导出全部；大文件可先改成例如 10000 做抽样
    constexpr std::size_t max_reads = 0;

    auto const input_path = std::filesystem::path(input_pod5);
    bool const input_is_dir = std::filesystem::is_directory(input_path);
    auto dir_name = input_path.filename().string();
    if (input_is_dir && dir_name.empty()) {
        dir_name = input_path.parent_path().filename().string();
    }
    // 单文件：int16_export；文件夹：int16_export_<输入文件夹名>
    auto const output_root = input_is_dir
        ? (std::filesystem::path(output_root_base).parent_path() / ("int16_export_" + dir_name))
              .string()
        : output_root_base;
    auto const files = collect_pod5_inputs(input_path);

    std::vector<Int16ExportSummary> summaries;
    summaries.reserve(files.size());
    for (auto const & file : files) {
        auto const stem = file.stem().string();
        auto const sizes_name =
            input_is_dir ? (stem + ".sizes.tsv") : std::string("sizes.tsv");
        auto const stats_name =
            input_is_dir ? (stem + ".stats.txt") : std::string("size_stats.txt");
        summaries.push_back(export_one_pod5_to_int16(
            file, output_root, max_reads, sizes_name, stats_name));
    }

    if (!input_is_dir) {
        return;
    }

    std::uint64_t total_pod5 = 0;
    std::uint64_t total_int16 = 0;
    std::size_t total_reads = 0;
    std::size_t total_exported = 0;
    bool all_full = true;
    auto const summary_path =
        (std::filesystem::path(output_root) / "summary.tsv").string();
    std::ofstream summary_tsv(summary_path);
    REQUIRE(summary_tsv.is_open());
    summary_tsv << "file\tpod5_bytes\tint16_bytes\ttotal_reads\texported\tratio\tspace_savings\tfull_export\n";

    std::cout << "\n[mytest4] Folder summary (" << summaries.size() << " pod5 files)\n";
    std::cout << std::fixed << std::setprecision(3);
    for (auto const & s : summaries) {
        total_pod5 += s.pod5_bytes;
        total_int16 += s.int16_bytes;
        total_reads += s.total_reads;
        total_exported += s.exported;
        all_full = all_full && s.full_export;
        auto const name = std::filesystem::path(s.input_pod5).filename().string();
        summary_tsv << name << '\t' << s.pod5_bytes << '\t' << s.int16_bytes << '\t'
                    << s.total_reads << '\t' << s.exported << '\t' << s.compression_ratio
                    << '\t' << s.space_savings << '\t' << (s.full_export ? 1 : 0) << '\n';
        std::cout << "  " << name << "  ratio=" << s.compression_ratio
                  << "  pod5=" << s.pod5_bytes << "  int16=" << s.int16_bytes << "\n";
    }

    double const folder_ratio = (total_pod5 > 0)
        ? (static_cast<double>(total_int16) / static_cast<double>(total_pod5))
        : 0.0;
    double const folder_savings = (total_int16 > 0)
        ? (100.0 * (1.0 - static_cast<double>(total_pod5) / static_cast<double>(total_int16)))
        : 0.0;
    std::cout << "Reads in folder / exported: " << total_reads << " / " << total_exported << "\n";
    std::cout << "Total POD5:  " << total_pod5 << " bytes\n";
    std::cout << "Total int16: " << total_int16 << " bytes\n";
    std::cout << "Compression ratio (int16/POD5): " << folder_ratio;
    if (all_full) {
        std::cout << "  (original int16 / pod5)\n";
        std::cout << "Space savings: " << folder_savings << " %\n";
    } else {
        std::cout << "  (exported int16 only; not comparable to full POD5)\n";
    }
}
