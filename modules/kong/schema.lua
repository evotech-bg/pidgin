-- Kong plugin schema for Pidgin JSON compression.

local typedefs = require "kong.db.schema.typedefs"

return {
    name = "pidgin",
    fields = {
        { protocols = typedefs.protocols_http },
        { consumer = typedefs.no_consumer },
        { config = {
            type = "record",
            fields = {
                { learn_samples = {
                    type = "integer",
                    default = 30,
                    gt = 0,
                    description = "Number of responses to observe before building a profile",
                }},
                { min_size = {
                    type = "integer",
                    default = 256,
                    gt = 0,
                    description = "Minimum response size in bytes for compression",
                }},
                { evolve_interval = {
                    type = "integer",
                    default = 500,
                    gt = 0,
                    description = "Re-learn interval for schema drift detection",
                }},
                { wellknown_path = {
                    type = "string",
                    default = "/.well-known/pidgin",
                    description = "Path prefix for serving profile JSON",
                }},
                { library_path = {
                    type = "string",
                    default = "libpidgin.so",
                    description = "Path to the libpidgin shared library",
                }},
            },
        }},
    },
}
