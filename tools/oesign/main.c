// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <openenclave/internal/elf.h>
#include <openenclave/internal/mem.h>
#include <openenclave/internal/properties.h>
#include <openenclave/internal/raise.h>
#include <openenclave/internal/sgxcreate.h>
#include <openenclave/internal/sgxsign.h>
#include <openenclave/internal/str.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "../host/enclave.h"

static const char* arg0;
int oedump(const char*);
int oesign(const char*, const char*, const char*);

OE_PRINTF_FORMAT(1, 2)
void Err(const char* format, ...)
{
    fprintf(stderr, "%s: ", arg0);

    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}

// Replace .so-extension with .signed.so. If there is no .so extension,
// append .signed.so.
static char* _make_signed_lib_name(const char* path)
{
    const char* p;
    mem_t buf = MEM_DYNAMIC_INIT;

    if ((!(p = strrchr(path, '.'))) || (strcmp(p, ".so") != 0))
        p = path + strlen(path);

    mem_append(&buf, path, (size_t)(p - path));
    mem_append(&buf, ".signed.so", 11);

    return (char*)mem_steal(&buf);
}

static oe_result_t _update_and_write_shared_lib(
    const char* path,
    const oe_sgx_enclave_properties_t* properties)
{
    oe_result_t rc = OE_FAILURE;
    oe_enclave_image_t oeimage;
    FILE* os = NULL;

    /* Open ELF file */
    if (oe_load_enclave_image(path, &oeimage) != OE_OK)
    {
        Err("cannot load ELF file: %s", path);
        goto done;
    }

    // Update or create a new .oeinfo section.
    if (oe_sgx_update_enclave_properties(
            &oeimage, OE_INFO_SECTION_NAME, properties) != OE_OK)
    {
        {
            Err("section doesn't exist: %s", OE_INFO_SECTION_NAME);
            goto done;
        }
    }

    /* Write new shared shared library */
    {
        char* p = _make_signed_lib_name(path);

        if (!p)
        {
            Err("bad shared library name: %s", path);
            goto done;
        }

        if (!(os = fopen(p, "wb")))
        {
            Err("failed to open: %s", p);
            goto done;
        }

        if (fwrite(oeimage.u.elf.elf.data, 1, oeimage.u.elf.elf.size, os) !=
            oeimage.u.elf.elf.size)
        {
            Err("failed to write: %s", p);
            goto done;
        }

        fclose(os);
        os = NULL;

        printf("Created %s\n", p);

        free(p);
    }

    rc = OE_OK;

done:

    if (os)
        fclose(os);

    oeimage.unload(&oeimage);

    return rc;
}

// Options loaded from .conf file. Uninitialized fields contain the maximum
// integer value for the corresponding type.
typedef struct _config_file_options
{
    bool debug;
    uint64_t num_heap_pages;
    uint64_t num_stack_pages;
    uint64_t num_tcs;
    uint16_t product_id;
    uint16_t security_version;
} ConfigFileOptions;

#define CONFIG_FILE_OPTIONS_INITIALIZER                                 \
    {                                                                   \
        .debug = false, .num_heap_pages = OE_UINT64_MAX,                \
        .num_stack_pages = OE_UINT64_MAX, .num_tcs = OE_UINT64_MAX,     \
        .product_id = OE_UINT16_MAX, .security_version = OE_UINT16_MAX, \
    }

/* Check whether the .conf file is missing required options */
static int _check_for_missing_options(const ConfigFileOptions* options)
{
    int ret = 0;

    if (options->num_heap_pages == OE_UINT64_MAX)
    {
        Err("%s: missing option: NumHeapPages", arg0);
        ret = -1;
    }

    if (options->num_stack_pages == OE_UINT64_MAX)
    {
        Err("%s: missing option: NumStackPages", arg0);
        ret = -1;
    }

    if (options->num_tcs == OE_UINT64_MAX)
    {
        Err("%s: missing option: NumTCS", arg0);
        ret = -1;
    }

    if (options->product_id == OE_UINT16_MAX)
    {
        Err("%s: missing option: ProductID", arg0);
        ret = -1;
    }

    if (options->security_version == OE_UINT16_MAX)
    {
        Err("%s: missing option: SecurityVersion", arg0);
        ret = -1;
    }

    return ret;
}

static int _load_config_file(const char* path, ConfigFileOptions* options)
{
    int rc = -1;
    FILE* is = NULL;
    int r;
    str_t str = STR_NULL_INIT;
    str_t lhs = STR_NULL_INIT;
    str_t rhs = STR_NULL_INIT;
    size_t line = 1;

    if (!(is = fopen(path, "rb")))
        goto done;

    if (str_dynamic(&str, NULL, 0) != 0)
        goto done;

    if (str_dynamic(&lhs, NULL, 0) != 0)
        goto done;

    if (str_dynamic(&rhs, NULL, 0) != 0)
        goto done;

    for (; (r = str_fgets(&str, is)) == 0; line++)
    {
        /* Remove leading and trailing whitespace */
        str_ltrim(&str, " \t");
        str_rtrim(&str, " \t\n\r");

        /* Skip comments and empty lines */
        if (str_ptr(&str)[0] == '#' || str_len(&str) == 0)
            continue;

        /* Split string about '=' character */
        if (str_split(&str, " \t=", &lhs, &rhs) != 0 || str_len(&lhs) == 0 ||
            str_len(&rhs) == 0)
        {
            Err("%s(%zu): syntax error", path, line);
            goto done;
        }

        /* Handle each setting */
        if (strcmp(str_ptr(&lhs), "Debug") == 0)
        {
            uint64_t value;

            // Debug must be 0 or 1
            if (str_u64(&rhs, &value) != 0 || (value > 1))
            {
                Err("%s(%zu): bad value for 'Debug'", path, line);
                goto done;
            }

            options->debug = (bool)value;
        }
        else if (strcmp(str_ptr(&lhs), "NumHeapPages") == 0)
        {
            uint64_t n;

            if (str_u64(&rhs, &n) != 0 || !oe_sgx_is_valid_num_heap_pages(n))
            {
                Err("%s(%zu): bad value for 'NumHeapPages'", path, line);
                goto done;
            }

            options->num_heap_pages = n;
        }
        else if (strcmp(str_ptr(&lhs), "NumStackPages") == 0)
        {
            uint64_t n;

            if (str_u64(&rhs, &n) != 0 || !oe_sgx_is_valid_num_stack_pages(n))
            {
                Err("%s(%zu): bad value for 'NumStackPages'", path, line);
                goto done;
            }

            options->num_stack_pages = n;
        }
        else if (strcmp(str_ptr(&lhs), "NumTCS") == 0)
        {
            uint64_t n;

            if (str_u64(&rhs, &n) != 0 || !oe_sgx_is_valid_num_tcs(n))
            {
                Err("%s(%zu): bad value for 'NumTCS'", path, line);
                goto done;
            }

            options->num_tcs = n;
        }
        else if (strcmp(str_ptr(&lhs), "ProductID") == 0)
        {
            uint16_t n;

            if (str_u16(&rhs, &n) != 0 || !oe_sgx_is_valid_product_id(n))
            {
                Err("%s(%zu): bad value for 'ProductID'", path, line);
                goto done;
            }

            options->product_id = n;
        }
        else if (strcmp(str_ptr(&lhs), "SecurityVersion") == 0)
        {
            uint16_t n;

            if (str_u16(&rhs, &n) != 0 || !oe_sgx_is_valid_security_version(n))
            {
                Err("%s(%zu): bad value for 'SecurityVersion'", path, line);
                goto done;
            }

            options->security_version = n;
        }
        else
        {
            Err("%s(%zu): unknown setting: %s", path, line, str_ptr(&rhs));
            goto done;
        }
    }

    rc = 0;

done:

    str_free(&str);
    str_free(&lhs);
    str_free(&rhs);

    if (is)
        fclose(is);

    return rc;
}

static int _load_pem_file(const char* path, void** data, size_t* size)
{
    int rc = -1;
    FILE* is = NULL;

    if (data)
        *data = NULL;

    if (size)
        *size = 0;

    /* Check parameters */
    if (!path || !data || !size)
        goto done;

    /* Get size of this file */
    {
        struct stat st;

        if (stat(path, &st) != 0)
            goto done;

        *size = (size_t)st.st_size;
    }

    /* Allocate memory. We add 1 to null terimate the file since the crypto
     * libraries require null terminated PEM data. */
    if (*size == SIZE_MAX)
        goto done;

    if (!(*data = (uint8_t*)malloc(*size + 1)))
        goto done;

    /* Open the file */
    if (!(is = fopen(path, "rb")))
        goto done;

    /* Read file into memory */
    if (fread(*data, 1, *size, is) != *size)
        goto done;

    /* Zero terminate the PEM data. */
    {
        uint8_t* data_tmp = (uint8_t*)*data;
        data_tmp[*size] = 0;
        *size += 1;
    }

    rc = 0;

done:

    if (rc != 0)
    {
        if (data && *data)
        {
            free(*data);
            *data = NULL;
        }

        if (size)
            *size = 0;
    }

    if (is)
        fclose(is);

    return rc;
}

// Load the SGX enclave properties from an enclave's .oeinfo section.
static oe_result_t _sgx_load_enclave_properties(
    const char* path,
    oe_sgx_enclave_properties_t* properties)
{
    oe_result_t result = OE_UNEXPECTED;
    oe_enclave_image_t oeimage;

    /* clear ELF magic */
    oeimage.u.elf.elf.magic = 0;

    if (properties)
        memset(properties, 0, sizeof(oe_sgx_enclave_properties_t));

    /* Check parameters */
    if (!path || !properties)
        OE_RAISE(OE_INVALID_PARAMETER);

    /* Load the ELF image */
    OE_CHECK(oe_load_enclave_image(path, &oeimage));

    /* Load the SGX enclave properties */
    OE_CHECK(
        oe_sgx_load_enclave_properties(
            &oeimage, OE_INFO_SECTION_NAME, properties));

    result = OE_OK;

done:

    if (oeimage.u.elf.elf.magic == ELF_MAGIC)
        oeimage.unload(&oeimage);

    return result;
}

/* Merge configuration file options into enclave properties */
void _merge_config_file_options(
    oe_sgx_enclave_properties_t* properties,
    const char* path,
    const ConfigFileOptions* options)
{
    bool initialized = false;

    /* Determine whether the properties are already initialized */
    if (properties->header.size == sizeof(oe_sgx_enclave_properties_t))
        initialized = true;

    /* Initialize properties if not already initialized */
    if (!initialized)
    {
        properties->header.size = sizeof(oe_sgx_enclave_properties_t);
        properties->header.enclave_type = OE_ENCLAVE_TYPE_SGX;
        properties->config.attributes = SGX_FLAGS_MODE64BIT;
    }

    /* Debug option is present */
    if (options->debug)
        properties->config.attributes |= SGX_FLAGS_DEBUG;

    /* If ProductID option is present */
    if (options->product_id != OE_UINT16_MAX)
        properties->config.product_id = options->product_id;

    /* If SecurityVersion option is present */
    if (options->security_version != OE_UINT16_MAX)
        properties->config.security_version = options->security_version;

    /* If NumHeapPages option is present */
    if (options->num_heap_pages != OE_UINT64_MAX)
        properties->header.size_settings.num_heap_pages =
            options->num_heap_pages;

    /* If NumStackPages option is present */
    if (options->num_stack_pages != OE_UINT64_MAX)
        properties->header.size_settings.num_stack_pages =
            options->num_stack_pages;

    /* If NumTCS option is present */
    if (options->num_tcs != OE_UINT64_MAX)
        properties->header.size_settings.num_tcs = options->num_tcs;
}

static const char _usage_gen[] =
    "Usage: %s <command> [options]\n"
    "\n"
    "Commands:\n"
    "    sign  -  Sign the specified enclave.\n"
    "    dump  -  Print out the Open Enclave metadata for the specified "
    "enclave.\n"
    "\n"
    "For help with a specific command, enter \"%s <command> -?\"\n";

static const char _usage_sign[] =
    "Usage: %s sign enclave_image config_file key_file\n"
    "\n"
    "Where:\n"
    "    enclave_image -- path of an enclave image file\n"
    "    config_file -- configuration file containing enclave properties\n"
    "    key_file -- private key file used to digitally sign the image\n"
    "\n"
    "Description:\n"
    "    This utility (1) injects runtime properties into an enclave image "
    "and\n"
    "    (2) digitally signs that image.\n"
    "\n"
    "    The properties are read from the <ConfigFile>. They override any\n"
    "    properties that were already defined inside the enclave image "
    "through\n"
    "    use of the OE_SET_ENCLAVE_SGX macro. These properties include:\n"
    "\n"
    "        Debug - whether enclave debug mode should be enabled (1) or not "
    "(0)\n"
    "        ProductID - the product identified number\n"
    "        SecurityVersion - the security version number\n"
    "        NumHeapPages - the number of heap pages for this enclave\n"
    "        NumStackPages - the number of stack pages for this enclave\n"
    "        NumTCS - the number of thread control structures for this "
    "enclave\n"
    "\n"
    "    The configuration file contains simple NAME=VALUE entries. For "
    "example:\n"
    "\n"
    "        Debug=1\n"
    "        NumHeapPages=1024\n"
    "\n"
    "    The key is read from <KeyFile> and contains a private RSA key in PEM\n"
    "    format. The keyfile must contain the following header.\n"
    "\n"
    "        -----BEGIN RSA PRIVATE KEY-----\n"
    "\n"
    "    The resulting image is written to <EnclaveImage>.signed.so.\n"
    "\n";

static const char _usage_dump[] =
    "\n"
    "Usage: %s dump enclave_image\n"
    "\n"
    "Where:\n"
    "    enclave_image -- path of an enclave image file\n"
    "\n"
    "Description:\n"
    "    This option dumps the oeinfo and signature information of an "
    "enclave\n";

int oesign(const char* enclave, const char* conffile, const char* keyfile)
{
    int ret = 1;
    oe_result_t result;
    oe_enclave_t enc;
    void* pem_data = NULL;
    size_t pem_size;
    ConfigFileOptions options = CONFIG_FILE_OPTIONS_INITIALIZER;
    oe_sgx_enclave_properties_t props;
    oe_sgx_load_context_t context;

    /* Load the configuration file */
    if (_load_config_file(conffile, &options) != 0)
    {
        Err("failed to load configuration file: %s", conffile);
        goto done;
    }

    /* Load the enclave properties from the enclave */
    {
        result = _sgx_load_enclave_properties(enclave, &props);

        if (result != OE_OK && result != OE_NOT_FOUND)
        {
            Err("failed to load enclave: %s: result=%s (%u)",
                enclave,
                oe_result_str(result),
                result);
            goto done;
        }

        /* If enclave properties not found, then options must be complete */
        if (result == OE_NOT_FOUND)
        {
            if (_check_for_missing_options(&options) != 0)
                goto done;
        }
    }

    /* Merge the configuration file options into the enclave properties */
    _merge_config_file_options(&props, conffile, &options);

    /* Check whether enclave properties are valid */
    {
        const char* field_name;

        if (oe_sgx_validate_enclave_properties(&props, &field_name) != OE_OK)
        {
            Err("invalid enclave property value: %s", field_name);
            goto done;
        }
    }

    /* Initialize the context parameters for measurement only */
    if (oe_sgx_initialize_load_context(
            &context, OE_SGX_LOAD_TYPE_MEASURE, props.config.attributes) !=
        OE_OK)
    {
        Err("oe_sgx_initialize_load_context() failed");
        goto done;
    }

    /* Build an enclave to obtain the MRENCLAVE measurement */
    if ((result = oe_sgx_build_enclave(&context, enclave, &props, &enc)) !=
        OE_OK)
    {
        Err("oe_sgx_build_enclave(): result=%s (%u)",
            oe_result_str(result),
            result);
        goto done;
    }

    /* Load private key into memory */
    if (_load_pem_file(keyfile, &pem_data, &pem_size) != 0)
    {
        Err("Failed to load file: %s", keyfile);
        goto done;
    }

    /* Initialize the SigStruct object */
    if ((result = oe_sgx_sign_enclave(
             &enc.hash,
             props.config.attributes,
             props.config.product_id,
             props.config.security_version,
             pem_data,
             pem_size,
             (sgx_sigstruct_t*)props.sigstruct)) != OE_OK)
    {
        Err("oe_sgx_sign_enclave() failed: result=%s (%u)",
            oe_result_str(result),
            result);
        goto done;
    }

    /* Create signature section and write out new file */
    if ((result = _update_and_write_shared_lib(enclave, &props)) != OE_OK)
    {
        Err("_update_and_write_shared_lib(): result=%s (%u)",
            oe_result_str(result),
            result);
        goto done;
    }

    ret = 0;

done:

    if (pem_data)
        free(pem_data);

    oe_sgx_cleanup_load_context(&context);

    return ret;
}

int dump_parser(const char* argv[])
{
    int ret = 1;
    const char* enclave;

    if (strcmp(argv[2], "-?") == 0)
    {
        fprintf(stderr, _usage_dump, argv[0]);
        exit(1);
    }
    else
    {
        if (strstr(argv[2], "_enc") != NULL)
        {
            enclave = argv[2];
            /* dump oeinfo and signature information */
            ret = oedump(enclave);
        }
        else
        {
            fprintf(stderr, _usage_gen, argv[0], argv[0]);
            exit(1);
        }
    }

    return ret;
}

int sign_parser(int argc, const char* argv[])
{
    int ret = 1;
    const char* enclave;
    const char* conffile;
    const char* keyfile;

    if (strcmp(argv[2], "-?") == 0)
    {
        fprintf(stderr, _usage_sign, argv[0]);
        exit(1);
    }
    else if (argc == 5)
    {
        if (strstr(argv[2], "enc") != NULL)
        {
            enclave = argv[2];
            if (strstr(argv[3], "conf") != NULL)
            {
                /* Collect arguments for signing*/
                conffile = argv[3];
                keyfile = argv[4];
            }
            else if (strstr(argv[3], "pem") != NULL)
            {
                /* Collect arguments for signing*/
                keyfile = argv[3];
                conffile = argv[4];
            }
            else
            {
                fprintf(stderr, _usage_gen, argv[0], argv[0]);
                exit(1);
            }
        }
        else if (strstr(argv[3], "enc") != NULL)
        {
            enclave = argv[3];
            if (strstr(argv[2], "conf") != NULL)
            {
                /* Collect arguments for signing*/
                conffile = argv[2];
                keyfile = argv[4];
            }
            else if (strstr(argv[2], "pem") != NULL)
            {
                /* Collect arguments for signing*/
                keyfile = argv[2];
                conffile = argv[4];
            }
            else
            {
                fprintf(stderr, _usage_gen, argv[0], argv[0]);
                exit(1);
            }
        }
        else if (strstr(argv[4], "enc") != NULL)
        {
            enclave = argv[4];
            if (strstr(argv[2], "conf") != NULL)
            {
                /* Collect arguments for signing*/
                conffile = argv[2];
                keyfile = argv[3];
            }
            else if (strstr(argv[2], "pem") != NULL)
            {
                /* Collect arguments for signing*/
                keyfile = argv[2];
                conffile = argv[3];
            }
            else
            {
                fprintf(stderr, _usage_gen, argv[0], argv[0]);
                exit(1);
            }
        }
        else
        {
            fprintf(stderr, _usage_gen, argv[0], argv[0]);
            exit(1);
        }
    }
    else
    {
        fprintf(stderr, _usage_gen, argv[0], argv[0]);
        exit(1);
    }

    ret = oesign(enclave, conffile, keyfile);

    return ret;
}

int arg_handler(int argc, const char* argv[])
{
    int ret = 1;
    if ((strcmp(argv[1], "dump") == 0))
        ret = dump_parser(argv);
    else if ((strcmp(argv[1], "sign") == 0))
        ret = sign_parser(argc, argv);
    else
    {
        fprintf(stderr, _usage_gen, argv[0], argv[0]);
        exit(1);
    }
    return ret;
}

int main(int argc, const char* argv[])
{
    arg0 = argv[0];
    int ret = 1;

    if (argc <= 2)
    {
        fprintf(stderr, _usage_gen, argv[0], argv[0]);
        exit(1);
    }

    ret = arg_handler(argc, argv);
    return ret;
}
