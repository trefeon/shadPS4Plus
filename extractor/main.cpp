// adapted from InstallDragDropPkg in main_window.cpp

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <string_view>
#include "core/file_format/pkg.h"
#include "core/file_format/psf.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "core/loader.h"

static bool IsSafeWindowsPathComponent(const std::string& component) {
	if (component.empty() || component == "." || component == ".." ||
		component.find_first_of("/\\:<>\"|?*") != std::string::npos ||
		std::any_of(component.begin(), component.end(), [](unsigned char c) { return c < 32; }) ||
		component.back() == '.' || component.back() == ' ') {
		return false;
	}

	std::string windows_name = component;
	std::transform(windows_name.begin(), windows_name.end(), windows_name.begin(),
		[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	const auto extension_separator = windows_name.find('.');
	const std::string device_name = windows_name.substr(0, extension_separator);
	const auto is_reserved_port = [&](std::string_view prefix) {
		if (!device_name.starts_with(prefix)) {
			return false;
		}
		const std::string_view suffix{device_name.data() + prefix.size(),
			device_name.size() - prefix.size()};
		return (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
			suffix == "\xB9" || suffix == "\xB2" || suffix == "\xB3" ||
			suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3";
	};
	return device_name != "CON" && device_name != "PRN" && device_name != "AUX" &&
		device_name != "NUL" && device_name != "CONIN$" && device_name != "CONOUT$" &&
		!is_reserved_port("COM") && !is_reserved_port("LPT");
}

int main(int argc, char** argv){
	std::filesystem::path file = "";
	std::filesystem::path output_folder_path = "";
	const bool check_type = argc == 3 && std::string("--check-type") == argv[2];
	int exit_code = 1;

	if(argc > 1){
		file += argv[1];
		if(argc > 2 && !check_type){
			output_folder_path += argv[2];
		}
	}

	if (Loader::DetectFileType(file) == Loader::FileTypes::Pkg) {
		std::cout << file << " is a valid PKG\n" << std::endl;
		PKG pkg = PKG();
		std::string failreason;

		if (!pkg.Open(file, failreason)) {
			std::cout << "Cannot open PKG file : " << failreason << std::endl;
		} else {
			PSF psf = PSF();
			if (!psf.Open(pkg.sfo)) {
				std::cout << "Could not read SFO." << std::endl;
			} else {
				auto category_value = psf.GetString("CATEGORY");
				if (!category_value.has_value() || category_value->empty()) {
					std::cout << "PSF file has no CATEGORY" << std::endl;
				} else {
					const std::string category{*category_value};
					const std::string pkg_type = pkg.GetPkgFlags();
					const std::string title_id{pkg.GetTitleID()};
					int type_code = 101;
					bool metadata_valid = true;
					std::string entitlement_label;

					if (pkg_type.contains("NON_GAME")) {
						std::cout << "Unsupported non-game package type" << std::endl;
						metadata_valid = false;
					} else if (pkg_type.contains("PATCH")) {
						std::cout << "pkg is a game update" << std::endl;
						type_code = 102;
					} else if (category == "ac") {
						std::cout << "pkg is a dlc" << std::endl;
						type_code = 103;
						auto content_id_value = psf.GetString("CONTENT_ID");
						if (!content_id_value.has_value()) {
							std::cout << "PSF file there is no CONTENT_ID" << std::endl;
							metadata_valid = false;
						} else {
							const auto content_id_parts =
								Common::SplitString(std::string{*content_id_value}, '-');
							if (content_id_parts.size() < 3 ||
								content_id_parts[1].size() <= title_id.size() ||
								content_id_parts[1].compare(0, title_id.size(), title_id) != 0 ||
								content_id_parts[1][title_id.size()] != '_') {
								metadata_valid = false;
							} else {
								entitlement_label = content_id_parts[2];
								if (!IsSafeWindowsPathComponent(entitlement_label)) {
									metadata_valid = false;
								}
							}
							if (!metadata_valid) {
								std::cout << "PSF CONTENT_ID has an invalid DLC entitlement label" << std::endl;
							}
						}
					} else if (category == "gd") {
						std::cout << "pkg is a base game" << std::endl;
					} else {
						std::cout << "Unsupported PKG category: " << category << std::endl;
						metadata_valid = false;
					}

					if (metadata_valid && !IsSafeWindowsPathComponent(title_id)) {
						std::cout << "PKG has an invalid title ID" << std::endl;
						metadata_valid = false;
					}

					if (metadata_valid) {
						if (check_type) {
							return type_code;
						}

						if (output_folder_path.empty()) {
							output_folder_path = file.parent_path();
						}
						output_folder_path /= title_id;
						if (type_code == 102) {
							output_folder_path += "-patch";
						} else if (type_code == 103) {
							output_folder_path /= entitlement_label;
						}

						std::cout << "Extracting pkg to " << output_folder_path << std::endl;
						if (!pkg.Extract(file, output_folder_path, failreason)) {
							std::cout << "Cannot extract PKG file : " << failreason << std::endl;
						} else {
							int nfiles = pkg.GetNumberOfFiles();
							bool extraction_succeeded = true;
							for(int i=0; i<nfiles; i++)
							{
								std::cout << "\r      Extracting file " << i+1 << " of " << nfiles;
								if (!pkg.ExtractFiles(i)) {
									std::cout << "\nCannot extract PKG file entry " << i << std::endl;
									extraction_succeeded = false;
									break;
								}
							}
							if (extraction_succeeded) {
								exit_code = 0;
							}
						}
					}
				}
			}
		}
	} else {
		std::cout << file << " doesn't appear to be a valid PKG file" << std::endl;
	}

	std::cout << "\nTHE END " << file << std::endl;
	if(argc == 2){
		std::cout << "\nPress [enter] to exit." << std::endl;
		std::cin.get();
	}

	return exit_code;
}
