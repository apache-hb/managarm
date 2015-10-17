
#include <frigg/cxx-support.hpp>
#include <frigg/glue-hel.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/protobuf.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "pci.hpp"
#include "mbus.frigg_pb.hpp"

void checkPciFunction(uint32_t bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = readPciHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	uint8_t header_type = readPciByte(bus, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		infoLogger->log() << "    Function " << function << ": Device" << frigg::EndLog();
	}else if((header_type & 0x7F) == 1) {
		uint8_t secondary = readPciByte(bus, slot, function, kPciSecondaryBus);
		infoLogger->log() << "    Function " << function
				<< ": PCI-to-PCI bridge to bus " << secondary << frigg::EndLog();
	}else{
		infoLogger->log() << "    Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F) << frigg::EndLog();
	}

	uint16_t device_id = readPciHalf(bus, slot, function, kPciDevice);
	uint8_t revision = readPciByte(bus, slot, function, kPciRevision);
	infoLogger->log() << "        Vendor: 0x" << frigg::logHex(vendor)
			<< ", device ID: 0x" << frigg::logHex(device_id)
			<< ", revision: " << revision << frigg::EndLog();
	
	uint8_t class_code = readPciByte(bus, slot, function, kPciClassCode);
	uint8_t sub_class = readPciByte(bus, slot, function, kPciSubClass);
	uint8_t interface = readPciByte(bus, slot, function, kPciInterface);
	infoLogger->log() << "        Class: " << class_code
			<< ", subclass: " << sub_class << ", interface: " << interface << frigg::EndLog();
	
	if((header_type & 0x7F) == 0) {
		PciDevice device(bus, slot, function, vendor, device_id, revision,
				class_code, sub_class, interface);
		
		// determine the BARs
		for(int i = 0; i < 6; i++) {
			uint32_t offset = kPciBar0 + i * 4;
			uint32_t bar = readPciWord(bus, slot, function, offset);
			if(bar == 0)
				continue;
			
			if((bar & 1) != 0) {
				uintptr_t address = bar & 0xFFFFFFFC;
				
				// write all 1s to the BAR and read it back to determine this its length
				writePciWord(bus, slot, function, offset, 0xFFFFFFFC);
				uint32_t mask = readPciWord(bus, slot, function, offset);
				writePciWord(bus, slot, function, offset, bar);
				uint32_t length = ~(mask & 0xFFFFFFFC) + 1;

				frigg::Vector<uintptr_t, Allocator> ports(*allocator);
				for(uintptr_t offset = 0; offset < length; offset++)
					ports.push(address + offset);

				device.bars[i].type = PciDevice::kBarIo;
				device.bars[i].length = length;
				HEL_CHECK(helAccessIo(ports.data(), ports.size(), &device.bars[i].handle));

				infoLogger->log() << "        I/O space BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " ports" << frigg::EndLog();
			}else if(((bar >> 1) & 3) == 0) {
				uint32_t address = bar & 0xFFFFFFF0;
				
				// write all 1s to the BAR and read it back to determine this its length
				writePciWord(bus, slot, function, offset, 0xFFFFFFF0);
				uint32_t mask = readPciWord(bus, slot, function, offset);
				writePciWord(bus, slot, function, offset, bar);
				uint32_t length = ~(mask & 0xFFFFFFF0) + 1;
				
				device.bars[i].type = PciDevice::kBarMemory;
				device.bars[i].length = length;
				HEL_CHECK(helAccessPhysical(address, length, &device.bars[i].handle));

				infoLogger->log() << "        32-bit memory BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " bytes" << frigg::EndLog();
			}else if(((bar >> 1) & 3) == 2) {
				assert(!"Handle 64-bit memory BARs");
			}else{
				assert(!"Unexpected BAR type");
			}
		}
		
		managarm::mbus::CntRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::mbus::CntReqType::REGISTER);
		
		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		mbusPipe.sendStringReq(serialized.data(), serialized.size(), 123, 0);

		uint8_t buffer[128];
		HelError error;
		size_t length;
		mbusPipe.recvStringRespSync(buffer, 128, eventHub, 123, 0, error, length);
		HEL_CHECK(error);
		
		managarm::mbus::SvrResponse<Allocator> response(*allocator);
		response.ParseFromArray(buffer, length);

		infoLogger->log() << "        ObjectID " << response.object_id() << frigg::EndLog();
	}
}

void checkPciDevice(uint32_t bus, uint32_t slot) {
	uint16_t vendor = readPciHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	infoLogger->log() << "Bus: " << bus << ", slot " << slot << frigg::EndLog();
	
	uint8_t header_type = readPciByte(bus, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			checkPciFunction(bus, slot, function);
	}else{
		checkPciFunction(bus, slot, 0);
	}
}

void checkPciBus(uint32_t bus) {
	for(uint32_t slot = 0; slot < 32; slot++)
		checkPciDevice(bus, slot);
}

void pciDiscover() {
	uintptr_t ports[] = { 0xCF8, 0xCF9, 0xCFA, 0xCFB, 0xCFC, 0xCFD, 0xCFE, 0xCFF };
	HelHandle io_handle;
	HEL_CHECK(helAccessIo(ports, 8, &io_handle));
	HEL_CHECK(helEnableIo(io_handle));

	checkPciBus(0);
}

