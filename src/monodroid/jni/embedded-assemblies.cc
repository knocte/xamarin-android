#include <host-config.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <cctype>
#include <libgen.h>
#include <cerrno>
#include <unistd.h>

#if defined (HAVE_LZ4)
#include <lz4.h>
#endif

#include <mono/metadata/assembly.h>
#include <mono/metadata/image.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/mono-debug.h>

#include "java-interop-util.h"

#include "monodroid.h"
#include "util.hh"
#include "embedded-assemblies.hh"
#include "globals.hh"
#include "monodroid-glue.hh"
#include "xamarin-app.hh"
#include "cpp-util.hh"
#include "startup-aware-lock.hh"

using namespace xamarin::android;
using namespace xamarin::android::internal;

// A utility class which allows us to manage memory allocated by `mono_guid_to_string` in an elegant way. We can create
// temporary instances of this class in calls to e.g. `log_debug` which are executed ONLY when debug logging is enabled
class MonoGuidString final
{
public:
	explicit MonoGuidString (const uint8_t *id) noexcept
	{
		guid = mono_guid_to_string (id);
	}

	~MonoGuidString ()
	{
		::free (guid);
	}

	const char* get () const noexcept
	{
		return guid;
	}

private:
	char *guid = nullptr;
};

void EmbeddedAssemblies::set_assemblies_prefix (const char *prefix)
{
	if (assemblies_prefix_override != nullptr)
		delete[] assemblies_prefix_override;
	assemblies_prefix_override = prefix != nullptr ? utils.strdup_new (prefix) : nullptr;
}

force_inline void
EmbeddedAssemblies::get_assembly_data (XamarinAndroidBundledAssembly const& e, uint8_t*& assembly_data, uint32_t& assembly_data_size)
{
#if defined (ANDROID) && defined (HAVE_LZ4) && defined (RELEASE)
	auto header = reinterpret_cast<const CompressedAssemblyHeader*>(e.data);
	if (header->magic == COMPRESSED_DATA_MAGIC) {
		if (XA_UNLIKELY (compressed_assemblies.descriptors == nullptr)) {
			log_fatal (LOG_ASSEMBLY, "Compressed assembly found but no descriptor defined");
			exit (FATAL_EXIT_MISSING_ASSEMBLY);
		}
		if (XA_UNLIKELY (header->descriptor_index >= compressed_assemblies.count)) {
			log_fatal (LOG_ASSEMBLY, "Invalid compressed assembly descriptor index %u", header->descriptor_index);
			exit (FATAL_EXIT_MISSING_ASSEMBLY);
		}

		CompressedAssemblyDescriptor &cad = compressed_assemblies.descriptors[header->descriptor_index];
		assembly_data_size = e.data_size - sizeof(CompressedAssemblyHeader);
		if (!cad.loaded) {
			if (XA_UNLIKELY (cad.data == nullptr)) {
				log_fatal (LOG_ASSEMBLY, "Invalid compressed assembly descriptor at %u: no data", header->descriptor_index);
				exit (FATAL_EXIT_MISSING_ASSEMBLY);
			}

			bool log_timing = utils.should_log (LOG_TIMING) && !(log_timing_categories & LOG_TIMING_BARE);
			timing_period decompress_time;
			if (XA_UNLIKELY (log_timing)) {
				decompress_time.mark_start ();
			}

			if (header->uncompressed_length != cad.uncompressed_file_size) {
				if (header->uncompressed_length > cad.uncompressed_file_size) {
					log_fatal (LOG_ASSEMBLY, "Compressed assembly '%s' is larger than when the application was built (expected at most %u, got %u). Assemblies don't grow just like that!", e.name, cad.uncompressed_file_size, header->uncompressed_length);
					exit (FATAL_EXIT_MISSING_ASSEMBLY);
				} else {
					log_debug (LOG_ASSEMBLY, "Compressed assembly '%s' is smaller than when the application was built. Adjusting accordingly.", e.name);
				}
				cad.uncompressed_file_size = header->uncompressed_length;
			}

			const char *data_start = reinterpret_cast<const char*>(e.data + sizeof(CompressedAssemblyHeader));
			int ret = LZ4_decompress_safe (data_start, reinterpret_cast<char*>(cad.data), static_cast<int>(assembly_data_size), static_cast<int>(cad.uncompressed_file_size));

			if (XA_UNLIKELY (log_timing)) {
				decompress_time.mark_end ();
				TIMING_LOG_INFO (decompress_time, "%s LZ4 decompression time", e.name);
			}

			if (ret < 0) {
				log_fatal (LOG_ASSEMBLY, "Decompression of assembly %s failed with code %d", e.name, ret);
				exit (FATAL_EXIT_MISSING_ASSEMBLY);
			}

			if (static_cast<uint64_t>(ret) != cad.uncompressed_file_size) {
				log_debug (LOG_ASSEMBLY, "Decompression of assembly %s yielded a different size (expected %lu, got %u)", e.name, cad.uncompressed_file_size, static_cast<uint32_t>(ret));
				exit (FATAL_EXIT_MISSING_ASSEMBLY);
			}
			cad.loaded = true;
		}
		assembly_data = reinterpret_cast<uint8_t*>(cad.data);
		assembly_data_size = cad.uncompressed_file_size;
	} else
#endif
	{
		assembly_data = e.data;
		assembly_data_size = e.data_size;
	}
}

#if defined (NET6)
MonoAssembly*
EmbeddedAssemblies::open_from_bundles (MonoAssemblyName* aname, MonoAssemblyLoadContextGCHandle alc_gchandle, [[maybe_unused]] MonoError *error)
{
	auto loader = [&] (uint8_t *assembly_data, uint32_t assembly_data_size, const char *name) -> MonoImage* {
		return mono_image_open_from_data_alc (
			alc_gchandle,
			reinterpret_cast<char*>(assembly_data),
			assembly_data_size,
			0 /* need_copy */,
			nullptr /* status */,
			name
		);
	};

	return open_from_bundles (aname, loader, false /* ref_only */);
}
#endif // def NET6

MonoAssembly*
EmbeddedAssemblies::open_from_bundles (MonoAssemblyName* aname, bool ref_only)
{
	auto loader = [&] (uint8_t *assembly_data, uint32_t assembly_data_size, const char *name) -> MonoImage* {
		return mono_image_open_from_data_with_name (
			reinterpret_cast<char*>(assembly_data),
			assembly_data_size,
			0,
			nullptr,
			ref_only,
			name
		);
	};

	return open_from_bundles (aname, loader, ref_only);
}

template<bool LogMapping>
force_inline void
EmbeddedAssemblies::map_runtime_file (XamarinAndroidBundledAssembly& file) noexcept
{
	md_mmap_info map_info = md_mmap_apk_file (file.apk_fd, file.data_offset, file.data_size, file.name);
	if (monodroidRuntime.is_startup_in_progress ()) {
		file.data = static_cast<uint8_t*>(map_info.area);
	} else {
		uint8_t *expected_null = nullptr;
		bool already_mapped = !__atomic_compare_exchange (
			/* ptr */              &file.data,
			/* expected */         &expected_null,
			/* desired */           reinterpret_cast<uint8_t**>(&map_info.area),
			/* weak */              false,
			/* success_memorder */  __ATOMIC_ACQUIRE,
			/* failure_memorder */  __ATOMIC_RELAXED
		);

		if (already_mapped) {
			log_debug (LOG_ASSEMBLY, "Assembly %s already mmapped by another thread, unmapping our copy", file.name);
			munmap (map_info.area, file.data_size);
			map_info.area = nullptr;
		}
	}

	if constexpr (LogMapping) {
		if (XA_UNLIKELY (utils.should_log (LOG_ASSEMBLY) && map_info.area != nullptr)) {
			const char *p = (const char*) file.data;

			std::array<char, 9> header;
			for (size_t j = 0; j < header.size () - 1; ++j)
				header[j] = isprint (p [j]) ? p [j] : '.';
			header [header.size () - 1] = '\0';

			log_info_nocheck (LOG_ASSEMBLY, "file-offset: % 8x  start: %08p  end: %08p  len: % 12i  zip-entry:  %s name: %s [%s]",
			                  (int) file.data_offset, file.data, file.data + file.data_size, (int) file.data_size, file.name, file.name, header.data ());
		}
	}
}

force_inline void
EmbeddedAssemblies::map_assembly (XamarinAndroidBundledAssembly& file) noexcept
{
	map_runtime_file<true> (file);
}

force_inline void
EmbeddedAssemblies::map_debug_data (XamarinAndroidBundledAssembly& file) noexcept
{
	map_runtime_file<false> (file);
}

force_inline MonoAssembly*
EmbeddedAssemblies::load_bundled_assembly (
	XamarinAndroidBundledAssembly& assembly,
	dynamic_local_string<SENSIBLE_PATH_MAX> const& name,
	dynamic_local_string<SENSIBLE_PATH_MAX> const& abi_name,
	std::function<MonoImage*(uint8_t*, uint32_t, const char*)> loader,
	bool ref_only) noexcept
{
	if (assembly.name == nullptr || assembly.name[0] == '\0') {
		return nullptr;
	}

	if (strcmp (assembly.name, name.get ()) != 0) {
		if (strcmp (assembly.name, abi_name.get ()) != 0) {
			return nullptr;
		} else {
			log_debug (LOG_ASSEMBLY, "open_from_bundles: found architecture-specific: '%s'", abi_name.get ());
		}
	}

	if (assembly.data == nullptr) {
		map_assembly (assembly);
	}

	uint8_t *assembly_data;
	uint32_t assembly_data_size;

	get_assembly_data (assembly, assembly_data, assembly_data_size);
	MonoImage *image = loader (assembly_data, assembly_data_size, name.get ());
	if (image == nullptr) {
		return nullptr;
	}

	if (have_and_want_debug_symbols) {
		uint32_t base_name_length = assembly.name_length - 3; // we need the trailing dot
		for (XamarinAndroidBundledAssembly& debug_file : *bundled_debug_data) {
			if (debug_file.name_length < base_name_length) {
				continue;
			}

			if (strncmp (debug_file.name, assembly.name, base_name_length) != 0) {
				continue;
			}

			if (debug_file.data == nullptr) {
				map_debug_data (debug_file);
			}

			if (debug_file.data != nullptr) {
				if (debug_file.data_size > std::numeric_limits<int>::max ()) {
					log_warn (LOG_ASSEMBLY, "Debug info file '%s' is too big for Mono to consume", debug_file.name);
				} else {
					mono_debug_open_image_from_memory (image, reinterpret_cast<const mono_byte*>(debug_file.data), static_cast<int>(debug_file.data_size));
				}
			}
			break;
		}
	}

	MonoImageOpenStatus status;
	MonoAssembly *a = mono_assembly_load_from_full (image, name.get (), &status, ref_only);
	if (a == nullptr) {
		return nullptr;
	}

#if !defined (NET6)
	// In dotnet the call is a no-op
	mono_config_for_assembly (image);
#endif
	return a;
}

MonoAssembly*
EmbeddedAssemblies::open_from_bundles (MonoAssemblyName* aname, std::function<MonoImage*(uint8_t*, size_t, const char*)> loader, bool ref_only)
{
	const char *culture = mono_assembly_name_get_culture (aname);
	const char *asmname = mono_assembly_name_get_name (aname);

	constexpr char path_separator[] = "/";
	dynamic_local_string<SENSIBLE_PATH_MAX> name;
	if (culture != nullptr && *culture != '\0') {
		name.append_c (culture);
		name.append (path_separator);
	}
	name.append_c (asmname);

	constexpr char dll_extension[] = ".dll";
	if (!utils.ends_with (name, dll_extension)) {
		name.append (dll_extension);
	}

	log_debug (LOG_ASSEMBLY, "open_from_bundles: looking for bundled name: '%s'", name.get ());

	dynamic_local_string<SENSIBLE_PATH_MAX> abi_name;
	abi_name
		.assign_c (BasicAndroidSystem::get_built_for_abi_name ())
		.append (path_separator)
		.append (name);

	MonoAssembly *a = nullptr;

	for (size_t i = 0; i < application_config.number_of_assemblies_in_apk; i++) {
		a = load_bundled_assembly (bundled_assemblies [i], name, abi_name, loader, ref_only);
		if (a != nullptr) {
			return a;
		}
	}

	if (extra_bundled_assemblies != nullptr) {
		for (XamarinAndroidBundledAssembly& assembly : *extra_bundled_assemblies) {
			a = load_bundled_assembly (assembly, name, abi_name, loader, ref_only);
			if (a != nullptr) {
				return a;
			}
		}
	}

	log_warn (LOG_ASSEMBLY, "open_from_bundles: failed to load assembly %s", name.get ());
	return nullptr;
}

#if defined (NET6)
MonoAssembly*
EmbeddedAssemblies::open_from_bundles (MonoAssemblyLoadContextGCHandle alc_gchandle, MonoAssemblyName *aname, [[maybe_unused]] char **assemblies_path, [[maybe_unused]] void *user_data, MonoError *error)
{
	return embeddedAssemblies.open_from_bundles (aname, alc_gchandle, error);
}
#else // def NET6
MonoAssembly*
EmbeddedAssemblies::open_from_bundles_refonly (MonoAssemblyName *aname, [[maybe_unused]] char **assemblies_path, [[maybe_unused]] void *user_data)
{
	return embeddedAssemblies.open_from_bundles (aname, true);
}
#endif // ndef NET6

MonoAssembly*
EmbeddedAssemblies::open_from_bundles_full (MonoAssemblyName *aname, [[maybe_unused]] char **assemblies_path, [[maybe_unused]] void *user_data)
{
	return embeddedAssemblies.open_from_bundles (aname, false);
}

void
EmbeddedAssemblies::install_preload_hooks_for_appdomains ()
{
	mono_install_assembly_preload_hook (open_from_bundles_full, nullptr);
#if !defined (NET6)
	mono_install_assembly_refonly_preload_hook (open_from_bundles_refonly, nullptr);
#endif // ndef NET6
}

#if defined (NET6)
void
EmbeddedAssemblies::install_preload_hooks_for_alc ()
{
	mono_install_assembly_preload_hook_v3 (
		open_from_bundles,
		nullptr /* user_data */,
		0 /* append */
	);
}
#endif // def NET6

template<typename Key, typename Entry, int (*compare)(const Key*, const Entry*), bool use_extra_size>
const Entry*
EmbeddedAssemblies::binary_search (const Key *key, const Entry *base, size_t nmemb, [[maybe_unused]] size_t extra_size)
{
	static_assert (compare != nullptr, "compare is a required template parameter");

	// This comes from the user code, so let's be civil
	if (key == nullptr) {
		log_warn (LOG_ASSEMBLY, "Key passed to binary_search must not be nullptr");
		return nullptr;
	}

	// This is a coding error on our part, crash!
	if (base == nullptr) {
		log_fatal (LOG_ASSEMBLY, "Map address not passed to binary_search");
		exit (FATAL_EXIT_MISSING_ASSEMBLY);
	}

	constexpr size_t size = sizeof(Entry);
	while (nmemb > 0) {
		const Entry *ret;
		if constexpr (use_extra_size) {
			ret = reinterpret_cast<const Entry*>(reinterpret_cast<const uint8_t*>(base) + ((size + extra_size) * (nmemb / 2)));
		} else {
			ret = base + (nmemb / 2);
		}

		int result = compare (key, ret);
		if (result < 0) {
			nmemb /= 2;
		} else if (result > 0) {
			if constexpr (use_extra_size) {
				base = reinterpret_cast<const Entry*>(reinterpret_cast<const uint8_t*>(ret) + size + extra_size);
			} else {
				base = ret + 1;
			}
			nmemb -= nmemb / 2 + 1;
		} else {
			return ret;
		}
	}

	return nullptr;
}

#if defined (DEBUG) || !defined (ANDROID)
int
EmbeddedAssemblies::compare_type_name (const char *type_name, const TypeMapEntry *entry)
{
	if (entry == nullptr)
		return 1;

	return strcmp (type_name, entry->from);
}

MonoReflectionType*
EmbeddedAssemblies::typemap_java_to_managed (const char *java_type_name)
{
	const TypeMapEntry *entry = nullptr;

	if (application_config.instant_run_enabled) {
		TypeMap *module;
		for (size_t i = 0; i < type_map_count; i++) {
			module = &type_maps[i];
			entry = binary_search<const char, TypeMapEntry, compare_type_name, false> (java_type_name, module->java_to_managed, module->entry_count);
			if (entry != nullptr)
				break;
		}
	} else {
		entry = binary_search<const char, TypeMapEntry, compare_type_name, false> (java_type_name, type_map.java_to_managed, type_map.entry_count);
	}

	if (XA_UNLIKELY (entry == nullptr)) {
		log_info (LOG_ASSEMBLY, "typemap: unable to find mapping to a managed type from Java type '%s'", java_type_name);
		return nullptr;
	}

	const char *managed_type_name = entry->to;
	if (managed_type_name == nullptr) {
		log_debug (LOG_ASSEMBLY, "typemap: Java type '%s' maps either to an open generic type or an interface type.", java_type_name);
		return nullptr;
	}
	log_debug (LOG_DEFAULT, "typemap: Java type '%s' corresponds to managed type '%s'", java_type_name, managed_type_name);

	MonoType *type = mono_reflection_type_from_name (const_cast<char*>(managed_type_name), nullptr);
	if (XA_UNLIKELY (type == nullptr)) {
		log_info (LOG_ASSEMBLY, "typemap: managed type '%s' (mapped from Java type '%s') could not be loaded", managed_type_name, java_type_name);
		return nullptr;
	}

	MonoReflectionType *ret = mono_type_get_object (utils.get_current_domain (), type);
	if (XA_UNLIKELY (ret == nullptr)) {
		log_warn (LOG_ASSEMBLY, "typemap: unable to instantiate managed type '%s'", managed_type_name);
		return nullptr;
	}

	return ret;
}
#else
MonoReflectionType*
EmbeddedAssemblies::typemap_java_to_managed (const char *java_type_name)
{
	TypeMapModule *module;
	const TypeMapJava *java_entry = binary_search<const char, TypeMapJava, compare_java_name, true> (java_type_name, map_java, java_type_count, java_name_width);
	if (java_entry == nullptr) {
		log_info (LOG_ASSEMBLY, "typemap: unable to find mapping to a managed type from Java type '%s'", java_type_name);
		return nullptr;
	}

	if (java_entry->module_index >= map_module_count) {
		log_warn (LOG_ASSEMBLY, "typemap: mapping from Java type '%s' to managed type has invalid module index", java_type_name);
		return nullptr;
	}

	module = const_cast<TypeMapModule*>(&map_modules[java_entry->module_index]);
	const TypeMapModuleEntry *entry = binary_search <uint32_t, TypeMapModuleEntry, compare_type_token> (&java_entry->type_token_id, module->map, module->entry_count);
	if (entry == nullptr) {
		log_info (LOG_ASSEMBLY, "typemap: unable to find mapping from Java type '%s' to managed type with token ID %u in module [%s]", java_type_name, java_entry->type_token_id, MonoGuidString (module->module_uuid).get ());
		return nullptr;
	}
	uint32_t type_token_id = java_entry->type_token_id;

	if (module->image == nullptr) {
		module->image = mono_image_loaded (module->assembly_name);
		if (module->image == nullptr) {
			// TODO: load
			log_error (LOG_ASSEMBLY, "typemap: assembly '%s' not loaded yet!", module->assembly_name);
		}

		if (module->image == nullptr) {
			log_error (LOG_ASSEMBLY, "typemap: unable to load assembly '%s' when looking up managed type corresponding to Java type '%s'", module->assembly_name, java_type_name);
			return nullptr;
		}
	}

	log_debug (LOG_ASSEMBLY, "typemap: java type '%s' corresponds to managed token id %u (0x%x)", java_type_name, type_token_id, type_token_id);
	MonoClass *klass = mono_class_get (module->image, static_cast<uint32_t>(type_token_id));
	if (klass == nullptr) {
		log_error (LOG_ASSEMBLY, "typemap: unable to find managed type with token ID %u in assembly '%s', corresponding to Java type '%s'", type_token_id, module->assembly_name, java_type_name);
		return nullptr;
	}

#if defined (NET6)
	// MonoVM in dotnet runtime doesn't use the `domain` parameter passed to `mono_type_get_object` (since AppDomains
	// are gone in NET6+), in fact, the function `mono_type_get_object` calls (`mono_type_get_object_checked`) itself
	// calls `mono_get_root_domain`. Thus, we can save on a one function call here by passing `nullptr`
	constexpr MonoDomain *domain = nullptr;
#else
	MonoDomain *domain = utils.get_current_domain ();
#endif
	MonoReflectionType *ret = mono_type_get_object (domain, mono_class_get_type (klass));
	if (ret == nullptr) {
		log_warn (LOG_ASSEMBLY, "typemap: unable to instantiate managed type with token ID %u in assembly '%s', corresponding to Java type '%s'", type_token_id, module->assembly_name, java_type_name);
		return nullptr;
	}

	return ret;
}

int
EmbeddedAssemblies::compare_java_name (const char *java_name, const TypeMapJava *entry)
{
	if (entry == nullptr || entry->java_name[0] == '\0') {
		return -1;
	}

	return strcmp (java_name, reinterpret_cast<const char*>(entry->java_name));
}
#endif

MonoReflectionType*
EmbeddedAssemblies::typemap_java_to_managed (MonoString *java_type)
{
	timing_period total_time;
	if (XA_UNLIKELY (utils.should_log (LOG_TIMING))) {
		timing = new Timing ();
		total_time.mark_start ();
	}

	if (XA_UNLIKELY (java_type == nullptr)) {
		log_warn (LOG_ASSEMBLY, "typemap: null 'java_type' passed to 'typemap_java_to_managed'");
		return nullptr;
	}

	c_unique_ptr<char> java_type_name {mono_string_to_utf8 (java_type)};
	if (XA_UNLIKELY (!java_type_name || *java_type_name == '\0')) {
		log_warn (LOG_ASSEMBLY, "typemap: empty Java type name passed to 'typemap_java_to_managed'");
		return nullptr;
	}

	MonoReflectionType *ret = typemap_java_to_managed (java_type_name.get ());

	if (XA_UNLIKELY (utils.should_log (LOG_TIMING))) {
		total_time.mark_end ();

		Timing::info (total_time, "Typemap.java_to_managed: end, total time");
	}

	return ret;
}

#if defined (DEBUG) || !defined (ANDROID)
inline const TypeMapEntry*
EmbeddedAssemblies::typemap_managed_to_java (const char *managed_type_name)
{
	const TypeMapEntry *entry = nullptr;

	if (application_config.instant_run_enabled) {
		TypeMap *module;
		for (size_t i = 0; i < type_map_count; i++) {
			module = &type_maps[i];
			entry = binary_search<const char, TypeMapEntry, compare_type_name, false> (managed_type_name, module->managed_to_java, module->entry_count);
			if (entry != nullptr)
				break;
		}
	} else {
		entry = binary_search<const char, TypeMapEntry, compare_type_name, false> (managed_type_name, type_map.managed_to_java, type_map.entry_count);
	}

	return entry;
}

inline const char*
EmbeddedAssemblies::typemap_managed_to_java ([[maybe_unused]] MonoType *type, MonoClass *klass, [[maybe_unused]] const uint8_t *mvid)
{
	c_unique_ptr<char> type_name {mono_type_get_name_full (type, MONO_TYPE_NAME_FORMAT_FULL_NAME)};
	MonoImage *image = mono_class_get_image (klass);
	const char *image_name = mono_image_get_name (image);
	size_t type_name_len = strlen (type_name.get ());
	size_t image_name_len = strlen (image_name);

	dynamic_local_string<SENSIBLE_PATH_MAX> full_name;
	full_name
		.append (type_name.get (), type_name_len)
		.append (", ")
		.append (image_name, image_name_len);

	const TypeMapEntry *entry = typemap_managed_to_java (full_name.get ());
	if (XA_UNLIKELY (entry == nullptr)) {
		log_info (LOG_ASSEMBLY, "typemap: unable to find mapping to a Java type from managed type '%s'", full_name.get ());
		return nullptr;
	}

	return entry->to;
}
#else
inline int
EmbeddedAssemblies::compare_type_token (const uint32_t *token, const TypeMapModuleEntry *entry)
{
	if (entry == nullptr) {
		log_fatal (LOG_ASSEMBLY, "typemap: compare_type_token: entry is nullptr");
		exit (FATAL_EXIT_MISSING_ASSEMBLY);
	}

	if (*token < entry->type_token_id)
		return -1;
	if (*token > entry->type_token_id)
		return 1;
	return 0;
}

inline int
EmbeddedAssemblies::compare_mvid (const uint8_t *mvid, const TypeMapModule *module)
{
	return memcmp (mvid, module->module_uuid, sizeof(module->module_uuid));
}

inline const char*
EmbeddedAssemblies::typemap_managed_to_java ([[maybe_unused]] MonoType *type, MonoClass *klass, const uint8_t *mvid)
{
	if (mvid == nullptr) {
		log_warn (LOG_ASSEMBLY, "typemap: no mvid specified in call to typemap_managed_to_java");
		return nullptr;
	}

	const TypeMapModule *map;
	size_t map_entry_count;
	map = map_modules;
	map_entry_count = map_module_count;

	const TypeMapModule *match = binary_search<uint8_t, TypeMapModule, compare_mvid> (mvid, map, map_entry_count);
	if (match == nullptr) {
		log_info (LOG_ASSEMBLY, "typemap: module matching MVID [%s] not found.", MonoGuidString (mvid).get ());
		return nullptr;
	}

	if (match->map == nullptr) {
		log_warn (LOG_ASSEMBLY, "typemap: module with MVID [%s] has no associated type map.", MonoGuidString (mvid).get ());
		return nullptr;
	}

	uint32_t token = mono_class_get_type_token (klass);
	log_debug (LOG_ASSEMBLY, "typemap: MVID [%s] maps to assembly %s, looking for token %d (0x%x), table index %d", MonoGuidString (mvid).get (), match->assembly_name, token, token, token & 0x00FFFFFF);
	// Each map entry is a pair of 32-bit integers: [TypeTokenID][JavaMapArrayIndex]
	const TypeMapModuleEntry *entry = binary_search <uint32_t, TypeMapModuleEntry, compare_type_token> (&token, match->map, match->entry_count);
	if (entry == nullptr) {
		if (match->duplicate_count > 0 && match->duplicate_map != nullptr) {
			log_debug (LOG_ASSEMBLY, "typemap: searching module [%s] duplicate map for token %u (0x%x)", MonoGuidString (mvid).get (), token, token);
			entry = binary_search <uint32_t, TypeMapModuleEntry, compare_type_token> (&token, match->duplicate_map, match->duplicate_count);
		}

		if (entry == nullptr) {
			log_info (LOG_ASSEMBLY, "typemap: type with token %d (0x%x) in module {%s} (%s) not found.", token, token, MonoGuidString (mvid).get (), match->assembly_name);
			return nullptr;
		}
	}

	uint32_t java_entry_count;
	java_entry_count = java_type_count;
	if (entry->java_map_index >= java_entry_count) {
		log_warn (LOG_ASSEMBLY, "typemap: type with token %d (0x%x) in module {%s} (%s) has invalid Java type index %u", token, token, MonoGuidString (mvid).get (), match->assembly_name, entry->java_map_index);
		return nullptr;
	}

	const char *ret;
	const TypeMapJava *java_entry = reinterpret_cast<const TypeMapJava*> (reinterpret_cast<const uint8_t*>(map_java) + ((sizeof(TypeMapJava) + java_name_width) * entry->java_map_index));
	ret = reinterpret_cast<const char*>(reinterpret_cast<const uint8_t*>(java_entry) + 8);

	if (XA_UNLIKELY (ret == nullptr)) {
		log_warn (LOG_ASSEMBLY, "typemap: empty Java type name returned for entry at index %u", entry->java_map_index);
	}

	log_debug (
		LOG_ASSEMBLY,
		"typemap: type with token %d (0x%x) in module {%s} (%s) corresponds to Java type '%s'",
		token,
		token,
		MonoGuidString (mvid).get (),
		match->assembly_name,
		ret
	);

	return ret;
}
#endif

const char*
EmbeddedAssemblies::typemap_managed_to_java (MonoReflectionType *reflection_type, const uint8_t *mvid)
{
	timing_period total_time;
	if (XA_UNLIKELY (utils.should_log (LOG_TIMING))) {
		timing = new Timing ();
		total_time.mark_start ();
	}

	MonoType *type = mono_reflection_type_get_type (reflection_type);
	if (type == nullptr) {
		log_warn (LOG_ASSEMBLY, "Failed to map reflection type to MonoType");
		return nullptr;
	}

	const char *ret = typemap_managed_to_java (type, mono_class_from_mono_type (type), mvid);

	if (XA_UNLIKELY (utils.should_log (LOG_TIMING))) {
		total_time.mark_end ();

		Timing::info (total_time, "Typemap.managed_to_java: end, total time");
	}

	return ret;
}

EmbeddedAssemblies::md_mmap_info
EmbeddedAssemblies::md_mmap_apk_file (int fd, uint32_t offset, size_t size, const char* filename)
{
	md_mmap_info file_info;
	md_mmap_info mmap_info;

	size_t pageSize        = static_cast<size_t>(utils.monodroid_getpagesize ());
	size_t offsetFromPage  = offset % pageSize;
	size_t offsetPage      = offset - offsetFromPage;
	size_t offsetSize      = size + offsetFromPage;

	mmap_info.area        = mmap (nullptr, offsetSize, PROT_READ, MAP_PRIVATE, fd, static_cast<off_t>(offsetPage));

	if (mmap_info.area == MAP_FAILED) {
		log_fatal (LOG_DEFAULT, "Could not `mmap` apk fd %d entry `%s`: %s", fd, filename, strerror (errno));
		exit (FATAL_EXIT_CANNOT_FIND_APK);
	}

	mmap_info.size  = offsetSize;
	file_info.area  = (void*)((const char*)mmap_info.area + offsetFromPage);
	file_info.size  = size;

	log_info (LOG_ASSEMBLY, "                       mmap_start: %08p  mmap_end: %08p  mmap_len: % 12u  file_start: %08p  file_end: %08p  file_len: % 12u      apk descriptor: %d  file: %s",
	          mmap_info.area, reinterpret_cast<int*> (mmap_info.area) + mmap_info.size, mmap_info.size,
	          file_info.area, reinterpret_cast<int*> (file_info.area) + file_info.size, file_info.size, fd, filename);

	return file_info;
}

void
EmbeddedAssemblies::gather_bundled_assemblies_from_apk (const char* apk, monodroid_should_register should_register)
{
	int fd;

	if ((fd = open (apk, O_RDONLY)) < 0) {
		log_error (LOG_DEFAULT, "ERROR: Unable to load application package %s.", apk);
		exit (FATAL_EXIT_NO_ASSEMBLIES);
	}
	log_info (LOG_ASSEMBLY, "APK %s FD: %d", apk, fd);

	zip_load_entries (fd, apk, should_register);
}

#if defined (DEBUG) || !defined (ANDROID)
ssize_t EmbeddedAssemblies::do_read (int fd, void *buf, size_t count)
{
	ssize_t ret;
	do {
		ret = ::read (
			fd,
			buf,
#if defined (WINDOWS)
			static_cast<unsigned int>(count)
#else
			count
#endif
		);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

template<typename H>
bool
EmbeddedAssemblies::typemap_read_header ([[maybe_unused]] int dir_fd, const char *file_type, const char *dir_path, const char *file_path, uint32_t expected_magic, H &header, size_t &file_size, int &fd)
{
	struct stat sbuf;
	int res;

#if __ANDROID_API__ < 21
	std::unique_ptr<char> full_file_path {utils.path_combine (dir_path, file_path)};
	res = stat (full_file_path.get (), &sbuf);
#else
	res = fstatat (dir_fd, file_path, &sbuf, 0);
#endif
	if (res < 0) {
		log_error (LOG_ASSEMBLY, "typemap: failed to stat %s file '%s/%s': %s", file_type, dir_path, file_path, strerror (errno));
		return false;
	}

	file_size = static_cast<size_t>(sbuf.st_size);
	if (file_size < sizeof (header)) {
		log_error (LOG_ASSEMBLY, "typemap: %s file '%s/%s' is too small (must be at least %u bytes)", file_type, dir_path, file_path, sizeof (header));
		return false;
	}

#if __ANDROID_API__ < 21
	fd = open (full_file_path.get (), O_RDONLY);
#else
	fd = openat (dir_fd, file_path, O_RDONLY);
#endif
	if (fd < 0) {
		log_error (LOG_ASSEMBLY, "typemap: failed to open %s file %s/%s for reading: %s", file_type, dir_path, file_path, strerror (errno));
		return false;
	}

	ssize_t nread = do_read (fd, &header, sizeof (header));
	if (nread <= 0) {
		if (nread < 0) {
			log_error (LOG_ASSEMBLY, "typemap: failed to read %s file header from '%s/%s': %s", file_type, dir_path, file_path, strerror (errno));
		} else {
			log_error (LOG_ASSEMBLY, "typemap: end of file while reading %s file header from '%s/%s'", file_type, dir_path, file_path);
		}

		return false;
	}

	if (header.magic != expected_magic) {
		log_error (LOG_ASSEMBLY, "typemap: invalid magic value in the %s file header from '%s/%s': expected 0x%X, got 0x%X", file_type, dir_path, file_path, expected_magic, header.magic);
		return false;
	}

	if (header.version != MODULE_FORMAT_VERSION) {
		log_error (LOG_ASSEMBLY, "typemap: incompatible %s format version. This build supports only version %u, file '%s/%s' uses version %u", file_type, MODULE_FORMAT_VERSION, dir_path, file_path, header.version);
		return false;
	}

	return true;
}

std::unique_ptr<uint8_t[]>
EmbeddedAssemblies::typemap_load_index (TypeMapIndexHeader &header, size_t file_size, int index_fd)
{
	size_t entry_size = header.module_file_name_width;
	size_t data_size = entry_size * type_map_count;
	if (sizeof(header) + data_size > file_size) {
		log_error (LOG_ASSEMBLY, "typemap: index file is too small, expected %u, found %u bytes", data_size + sizeof(header), file_size);
		return nullptr;
	}

	auto data = std::make_unique<uint8_t[]> (data_size);
	ssize_t nread = do_read (index_fd, data.get (), data_size);
	if (nread != static_cast<ssize_t>(data_size)) {
		log_error (LOG_ASSEMBLY, "typemap: failed to read %u bytes from index file. %s", data_size, strerror (errno));
		return nullptr;
	}

	uint8_t *p = data.get ();
	for (size_t i = 0; i < type_map_count; i++) {
		type_maps[i].assembly_name = reinterpret_cast<char*>(p);
		p += entry_size;
	}

	return data;
}

std::unique_ptr<uint8_t[]>
EmbeddedAssemblies::typemap_load_index (int dir_fd, const char *dir_path, const char *index_path)
{
	log_debug (LOG_ASSEMBLY, "typemap: loading TypeMap index file '%s/%s'", dir_path, index_path);

	TypeMapIndexHeader header;
	size_t file_size;
	int fd = -1;
	std::unique_ptr<uint8_t[]> data;

	if (typemap_read_header (dir_fd, "TypeMap index", dir_path, index_path, MODULE_INDEX_MAGIC, header, file_size, fd)) {
		type_map_count = header.entry_count;
		type_maps = new TypeMap[type_map_count]();
		data = typemap_load_index (header, file_size, fd);
	}

	if (fd >= 0)
		close (fd);

	return data;
}

bool
EmbeddedAssemblies::typemap_load_file (BinaryTypeMapHeader &header, const char *dir_path, const char *file_path, int file_fd, TypeMap &module)
{
	size_t alloc_size = ADD_WITH_OVERFLOW_CHECK (size_t, header.assembly_name_length, 1);
	module.assembly_name = new char[alloc_size];

	ssize_t nread = do_read (file_fd, module.assembly_name, header.assembly_name_length);
	if (nread != static_cast<ssize_t>(header.assembly_name_length)) {
		log_error (LOG_ASSEMBLY, "tyemap: failed to read map assembly name from '%s/%s': %s", dir_path, file_path, strerror (errno));
		return false;
	}

	module.assembly_name [header.assembly_name_length] = 0;
	module.entry_count = header.entry_count;

	log_debug (
		LOG_ASSEMBLY,
		"typemap: '%s/%s':: entry count == %u; Java name field width == %u; Managed name width == %u; assembly name length == %u; assembly name == %s",
		dir_path, file_path, header.entry_count, header.java_name_width, header.managed_name_width, header.assembly_name_length, module.assembly_name
	);

	// [name][index]
	size_t java_entry_size = header.java_name_width + sizeof(uint32_t);
	size_t managed_entry_size = header.managed_name_width + sizeof(uint32_t);
	size_t data_size = ADD_WITH_OVERFLOW_CHECK (
		size_t,
		header.entry_count * java_entry_size,
		header.entry_count * managed_entry_size
	);

	module.data = new uint8_t [data_size];
	nread = do_read (file_fd, module.data, data_size);
	if (nread != static_cast<ssize_t>(data_size)) {
		log_error (LOG_ASSEMBLY, "tyemap: failed to read map data from '%s/%s': %s", dir_path, file_path, strerror (errno));
		return false;
	}

	module.java_to_managed = new TypeMapEntry [module.entry_count];
	module.managed_to_java = new TypeMapEntry [module.entry_count];

	uint8_t *java_start = module.data;
	uint8_t *managed_start = module.data + (module.entry_count * java_entry_size);
	uint8_t *java_pos = java_start;
	uint8_t *managed_pos = managed_start;
	TypeMapEntry *cur;

	constexpr uint32_t INVALID_TYPE_INDEX = std::numeric_limits<uint32_t>::max ();
	for (size_t i = 0; i < module.entry_count; i++) {
		cur = &module.java_to_managed[i];
		cur->from = reinterpret_cast<char*>(java_pos);

		uint32_t idx;
		// This might seem slow but it is in fact compiled into a single instruction and is safe when loading the 32-bit
		// integer from unaligned memory
		memcpy (&idx, java_pos + header.java_name_width, sizeof (idx));
		if (idx < INVALID_TYPE_INDEX) {
			cur->to = reinterpret_cast<char*>(managed_start + (managed_entry_size * idx));
		} else {
			// Ignore the type mapping
			cur->to = nullptr;
		}
		java_pos += java_entry_size;

		cur = &module.managed_to_java[i];
		cur->from = reinterpret_cast<char*>(managed_pos);

		memcpy (&idx, managed_pos + header.managed_name_width, sizeof (idx));
		cur->to = reinterpret_cast<char*>(java_start + (java_entry_size * idx));
		managed_pos += managed_entry_size;
	}

	return true;
}

bool
EmbeddedAssemblies::typemap_load_file (int dir_fd, const char *dir_path, const char *file_path, TypeMap &module)
{
	log_debug (LOG_ASSEMBLY, "typemap: loading TypeMap file '%s/%s'", dir_path, file_path);

	bool ret = true;
	BinaryTypeMapHeader header;
	size_t file_size;
	int fd = -1;

	module.java_to_managed = nullptr;
	module.managed_to_java = nullptr;

	if (!typemap_read_header (dir_fd, "TypeMap", dir_path, file_path, MODULE_MAGIC_NAMES, header, file_size, fd)) {
		ret = false;
		goto cleanup;
	}

	ret = typemap_load_file (header, dir_path, file_path, fd, module);

  cleanup:
	if (fd >= 0)
		close (fd);

	if (!ret) {
		delete[] module.java_to_managed;
		module.java_to_managed = nullptr;
		delete[] module.managed_to_java;
		module.managed_to_java = nullptr;
	}

	return ret;
}

void
EmbeddedAssemblies::try_load_typemaps_from_directory (const char *path)
{
	if (!application_config.instant_run_enabled) {
		log_info (LOG_ASSEMBLY, "typemap: instant run disabled, not loading type maps from storage");
		return;
	}

	std::unique_ptr<char> dir_path {utils.path_combine (path, "typemaps")};
	monodroid_dir_t *dir;
	if ((dir = utils.monodroid_opendir (dir_path.get ())) == nullptr) {
		log_warn (LOG_ASSEMBLY, "typemap: could not open directory: `%s`", dir_path.get ());
		return;
	}

	int dir_fd;
#if __ANDROID_API__ < 21
	dir_fd = -1;
#else
	dir_fd = dirfd (dir);
#endif

	constexpr char index_name[] = "typemap.index";

	// The pointer must be stored here because, after index is loaded, module.assembly_name points into the index data
	// and must be valid until after the actual module file is loaded.
	std::unique_ptr<uint8_t[]> index_data = typemap_load_index (dir_fd, dir_path.get (), index_name);
	if (!index_data) {
		log_fatal (LOG_ASSEMBLY, "typemap: unable to load TypeMap data index from '%s/%s'", dir_path.get (), index_name);
		exit (FATAL_EXIT_NO_ASSEMBLIES); // TODO: use a new error code here
	}

	for (size_t i = 0; i < type_map_count; i++) {
		TypeMap *module = &type_maps[i];
		if (!typemap_load_file (dir_fd, dir_path.get (), module->assembly_name, *module)) {
			continue;
		}
	}

	utils.monodroid_closedir (dir);
}
#endif

size_t
EmbeddedAssemblies::register_from (const char *apk_file, monodroid_should_register should_register)
{
	size_t prev  = bundled_assembly_index;

	gather_bundled_assemblies_from_apk (apk_file, should_register);

	log_info (LOG_ASSEMBLY, "Package '%s' contains %i assemblies", apk_file, bundled_assembly_index - prev);

	return bundled_assembly_index;
}
