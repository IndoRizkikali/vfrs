1c1
< # VFRS Switched Virtual Circuit (SVC) Implementation Plan
---
> # VFRS Switched Virtual Circuit (SVC) Implementation Plan - Reviewed and Annotated Version
36a37,53
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > 1: Confirmed, but actually, CUG and call barring will be implemented in future, in different development stages (in separate stages after this stage).  
> > 3: According to Clause 10.6.2 of X.36, only call reference values (CRV) of two octets (representing a 15-bit value) are supported. Even if the active call reference number is small (like 0x05) and easily fit inside a single byte, the encoding of the call reference value must always use exactly two octets (eg 0x00 0x05). This means that VFRS if possible operate in strict compliance with X.36 and only support 1-byte CRV on per-interface basis if the DTE connected to that interface use that format. This also require CRV translator to translate between 1-byte CRV and 2-bytes CRV.  
> > 4: For static configurable allocation, default for 10-bit DLCI: 512 up to 991. For 23-bit DLCI: 512 up to 991, and then 1024 up to 8388607 (or 4194304).  
> > 5: See below.  
> > 6: Initially, manual per-port basis, then later auto-assignment (global + grouped numbering), for a total of 3 options (see "C:\Users\rizki\Programming\vfrns\vfrns_numbering_plan_additional.md"). All of them prohibit all-zeros (see no 13).  
> > 10: Each DLCI isn't equal in priority, so each PVC and SVC DLCIs have different CIR, Bc, Be, and other QoS parameters, and these parameters are independent of each other. They still are limited by the access rate as well QoS parameters on the interface (if not set explicitly for each PVC and the default per-interface SVC configuration) and globally. Review carefully the QoS codes.  
> > 12: If possible, use asynchronous event signaling in addition of the current polling, so the sensitive timer wouldn't easily expired because lateness, given the Dynamips VM are sometimes clocking slower than the host.  
> > 13: For enable SPVC services, all VFRS node must also support behaving as DTE, which make this very complex. This is also why all exchanges are assigned subscriber number of all-zero. See "C:\Users\rizki\Programming\vfrns\vfrns_numbering_plan_new.md".  
> > 16: According to Clauses 10.7.1.2 and 10.7.2.1 of X.36:  
> > Calling party number screening and presentation: The screening and presentation indicators of the calling party number information element shall be transmitted to the remote DTE and the presentation indicator (octet 3a bits 6 and 7) shall be coded Presentation allowed.  
> > Connected number screening and presentation: If the called DTE provides a connected number information element in the CONNECT message, the screening and presentation indicators of the connected number information element will be transmitted to the originating interface and the presentation indicator (octet 3a bits 6 and 7) shall be coded Presentation allowed.  
> > 21: Technically, there absolutely no reason to require 3/4-octet addressing, and both X.36 and X.76 mandates (require) only 10-bit DLCI support. 23-bit DLCI support is an optional capability. 3-octet DLCI is not specified under these standards, even Q.922 do. The default is 10-bit DLCI.  
> > 23: The call deflection/redirection capability is a non-standardized (or indeed, standard-violating) feature with 'proprietary' (in sense of non-standardization, not the software licensing one!) extension to X.36 and X.76 protocol and as such by default unavailable for all interfaces. Only DTEs that can support it are VFRAD and VFRTAC. Cisco IOS is likely to choke on it.  
> > In addition to above, it is possible for implement SVC-based multicasting. For standard-compliance and compatibility reasons, only statically-configured multicast service is currently supported. See "C:\Users\rizki\Programming\vfrns\standards-and-specifications\notebooklm2.md"  
> > For those without any annotions, this because I assume it is fine.
> 
306a324,329
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > 1. For UNI, all 10 message types (Alerting and Connect Acknowledgement types are not longer listed and used in the 2003 version of X.36, but for compatibility reasons with older DTEs is permitted with understanding that they are compliant with the last version with these message types) and 18 information elements must be defined. For NNI, all 9 message types and 23 information elements must be defined. Full support for all message types, as well the critical information elements must be provided (for Generic Application Transport IE, this fall under FRF.10.1 and as such only can be implemented in the NNI phase when I completing transcribing it to Markdown). This why it is partially syncronized - some of the IEs are for later phases, but are prepared now.  
> > 2. All of the UNI DCE-side state machine (including the Restart state machine) must be fully implemented. Later on in the NNI phase, all of the NNI state machine are also fully implemented too.  
> > 3. X.121 is used for convenience, the future goal are E.164 and X.121, including interworking support between these different numbering schemes.  
> > 4. Update the maximum constants to at least twice of the current values.
> 
325a349,354
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > See "C:\Users\rizki\Programming\vfrns\vfrns_numbering_plan_new.md" and "C:\Users\rizki\Programming\vfrns\vfrns_numbering_plan_additional.md".  
> > The planned format is `svc_addr  <port_name> [x121|e164] <primary_sub_number> [alias=x121|e164,<alias_sub_number>]`.  
> > 1. See point 3 at the above annotions. Only X.121 number type fully support number expansion.  
> > 2. The switches are assigned all-zeros subscriber number. Such number cannot be assigned to any ports, but can be dialed by anyone from any ports (including inter-switch SVC). This number is used to: enable in-band SVC-based signaling and management for NNI and UNI, identifying edge switches in SPVC establishment and management and support correct inter-switch path-finding and routing for them, providing additional services over SVC, and for any other purposes.
> 
350a380,382
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > Cisco IOS is using the full international number format for Called party number, Calling party number, and Connected number IEs. X.36 specifies 5 different types of numbering (international, national, network-specific, complementary, alternative). For national numbers, it must converted into international number when transmitted into NNI/inter-switch networks with prepending them with the outgoing VFRS instance's own DNIC/DCC.
> 
401a434,436
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > DO NOT USE `svc_default_cir/bc/be`. Use the `defaults` instead. See "C:\Users\rizki\Programming\vfrns\vfr_switch\example_config.conf".
> 
484a520,522
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > Make sure all of the critical IEs for both NNI and UNI are fully covered. Those that only make sense for DTEs are can be ignored for now.
> 
583a622,624
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > Reread the Clauses 10.7 to 10.11 of the X.36.
> 
704a746,748
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > THE ANOTATIONS FOR PHASE 2 ARE ONLY ADDED AFTER THE PHASE 1 IS COMPLETED. DO NOT CONTINUE UNTIL THE PHASE 2 ANOTATIONS ARE RELEASED AND THE PLAN UPDATED.
> 
828a873,875
> 
> > [!NOTE] **ANNOTATIONS and CORRECTIONS**:  
> > THE ANOTATIONS FOR PHASE 3 ARE ONLY ADDED AFTER THE PHASE 2 IS COMPLETED. DO NOT CONTINUE UNTIL THE PHASE 3 ANOTATIONS ARE RELEASED AND THE PLAN UPDATED.
