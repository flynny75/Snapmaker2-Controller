#include "../inc/MarlinConfig.h"


#include "../Marlin.h"
#include "temperature.h"
#include "planner.h"
#include "executermanager.h"
#include "motion.h"
#include "../gcode/gcode.h"
#include "../feature/bedlevel/bedlevel.h"
#include "../module/configuration_store.h"
#include "../gcode/parser.h"
#include "../SnapScreen/Screen.h"
#include "periphdevice.h"
#include <EEPROM.h>
#include "../sd/cardreader.h"
#include "printcounter.h"
#include "StatusControl.h"

#include "PowerPanic.h"

#define FLASH_PAGE_SIZE	2048
#define FLASH_RECORD_PAGES	(MARLIN_POWERPANIC_SIZE / 2048)
#define RECORD_COUNT_PER_PAGE	(FLASH_PAGE_SIZE / (sizeof(strPowerPanicSave) + 8))

PowerPanic PowerPanicData;

#if ENABLED(VARIABLE_G0_FEEDRATE)
  extern float saved_g0_feedrate_mm_s;
  extern float saved_g1_feedrate_mm_s;
#endif

#if USE_EXECUTE_COMMANDS_IMMEDIATE
#define process_cmd_imd(str) do{parser.parse(str), _
                                gcode.process_parsed_command( _
                                false \
                                );}while(0);
#else
#define process_cmd_imd(str) do{parser.parse(str), gcode.process_parsed_command( );}while(0);
#endif
                                
 /**
 *Load power panic data from flash
 *return :true if exist valid power panic data, or else false
 */
bool PowerPanic::Load(void)
{
  uint8_t *pSrcBuff, *pDstBuff;
	uint32_t i;
	uint32_t tmpIndex;
	uint32_t addr;
	uint32_t RecordSize;
	uint32_t TotalCount;
	uint32_t Flag;
  bool PowerpanicExist = true;

	//记录大小
	RecordSize = (sizeof(strPowerPanicSave) + 8);
	//记录总空间
	TotalCount = RECORD_COUNT_PER_PAGE * FLASH_RECORD_PAGES;
  tmpIndex = 0;

	//查找空块
	for(i=0;i<TotalCount;i++)
	{
		//计算地址
		addr = (i / RECORD_COUNT_PER_PAGE) * 2048 + (i % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC;
		//读取标置
		Flag = *((uint32_t*)addr);
		//空块
		if(Flag == 0xffffffff)
		{
			//
			tmpIndex = (i + TotalCount - 1) % TotalCount;
			break;
		}
	}

	//查找最后一个非空块
	for(i=0;i<TotalCount;i++)
	{
		//计算地址
		addr = (tmpIndex / RECORD_COUNT_PER_PAGE) * 2048 + (tmpIndex % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC;
		Flag = *((uint32_t*)addr);
		//非空块
		if(Flag != 0xffffffff)
			break;
		//
		tmpIndex = (tmpIndex + TotalCount - 1) % TotalCount;
	}

	//读取标置位
	Flag = *((uint32_t*)addr);
	//Flash  是空的，表示没有记录
	if(Flag == 0xffffffff)
	{
		//当前位置作为写入位置
		WriteIndex = 0;
		//标置无效
		Data.Valid = 0;
		//标志断电错误标置
		PowerpanicExist = false;
	}
	else
	{
		//地址
		addr = (tmpIndex / RECORD_COUNT_PER_PAGE) * 2048 + (tmpIndex % RECORD_COUNT_PER_PAGE) * RecordSize + 8 + FLASH_MARLIN_POWERPANIC;
		//读取数据 
		pSrcBuff = (uint8_t*)addr;
		pDstBuff = (uint8_t*)&Data;
		for(uint32_t i=0;i<sizeof(strPowerPanicSave);i++)
			*pDstBuff++ = *pSrcBuff++;

		//校验
		uint32_t Checksum;
		uint32_t tmpChecksum;
		tmpChecksum = Data.CheckSum;
		Data.CheckSum = 0;
		Checksum = 0;
		pSrcBuff = (uint8_t*)&Data;
		for(uint32_t i=0;i<sizeof(strPowerPanicSave);i++)
			Checksum += pSrcBuff[i];
		//Checksum = Checksum - (uint8_t)(Data.CheckSum >> 24) - (uint8_t)(Data.CheckSum >> 16) - (uint8_t)(Data.CheckSum >> 8) - 
		// (uint8_t)(Data.CheckSum);
		
		//校验失败
		if(Checksum != tmpChecksum)
		{
			//清除标志
			FLASH_Unlock();
			addr = (tmpIndex / RECORD_COUNT_PER_PAGE) * 2048 + (WriteIndex % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC;
			FLASH_ProgramWord(addr, 0);
			FLASH_Lock();
			//标置无效
			Data.Valid = 0;
			//标志断电错误标置
			PowerpanicExist = false;
		}
		//校验成功
		else
		{
			//标置有效
			Data.Valid = 1;
		}

		//地址
		addr = (tmpIndex / RECORD_COUNT_PER_PAGE) * 2048 + (tmpIndex % RECORD_COUNT_PER_PAGE) * RecordSize + 4 + FLASH_MARLIN_POWERPANIC;
		//读取标置位
		Flag = *((uint32_t*)addr);
		//完整标置无效
		if(Flag != 0x5555)
		{
			Data.Valid = 0;
			//标志断电错误标置
			PowerpanicExist = false;
		}	
		
		//下一个位置作为写入位置
		WriteIndex = (tmpIndex + 1) % TotalCount;
		//首地址
		if((WriteIndex % RECORD_COUNT_PER_PAGE) == 0)
		{
			//控除FLASH
			FLASH_Unlock();
			addr = (tmpIndex / RECORD_COUNT_PER_PAGE) * 2048 + (WriteIndex % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC;
			FLASH_ErasePage(addr);
			FLASH_Lock();
		}
	}
  return PowerpanicExist;
}

 /**
 *Restore the power panic data
 */
void PowerPanic::Restore(void)
{
  uint32_t addr;
	uint32_t RecordSize;
	uint32_t checksum;
	uint32_t u32data;
	uint8_t *pBuff;
	
	//记录大小
	RecordSize = (sizeof(strPowerPanicSave) + 8);

	//写一半标识
	addr = (WriteIndex / RECORD_COUNT_PER_PAGE) * 2048 + (WriteIndex % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC;
	u32data = 0x5555;
	FLASH_Unlock();
	FLASH_ProgramWord(addr, u32data);

	//地址
	addr = (WriteIndex / RECORD_COUNT_PER_PAGE) * 2048 + (WriteIndex % RECORD_COUNT_PER_PAGE) * RecordSize + 8 + FLASH_MARLIN_POWERPANIC;
	
	pBuff = (uint8_t*)&Data;
	Data.CheckSum = 0;
	checksum = 0;
	for(uint32_t i=0;i<sizeof(strPowerPanicSave);i++)
		checksum += pBuff[i];
	Data.CheckSum = checksum;

	
	for(uint32_t i=0;i<sizeof(strPowerPanicSave);i=i+4)
	{
		u32data = (pBuff[i + 3] << 24) | (pBuff[i + 2] << 16) | (pBuff[i + 1] << 8) | pBuff[i];
		FLASH_ProgramWord(addr, u32data);
		addr = addr + 4;
	}

	//写完整标识
	addr = (WriteIndex / RECORD_COUNT_PER_PAGE) * 2048 + (WriteIndex % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC + 4;
	u32data = 0x5555;
	FLASH_ProgramWord(addr, u32data);
	FLASH_Lock();
}

 /**
 *Clear all power panic data
 */
void PowerPanic::ClearPowerPanicData(void)
{
  uint32_t addr;

	addr = FLASH_MARLIN_POWERPANIC;
	FLASH_Unlock();
	for(int i=0;i<FLASH_RECORD_PAGES;i++)
	{
		FLASH_ErasePage(addr);
		addr += 2048;
	}
	FLASH_Lock();
}

 /**
 *discard power panic data
 */
void PowerPanic::MaskPowerPanicData(void)
{
	uint32_t RecordSize;
	uint32_t TotalCount;
	uint32_t addr;
	uint16_t preIndex;
	
	//记录大小
	RecordSize = (sizeof(strPowerPanicSave) + 8);
	//记录总空间
	TotalCount = RECORD_COUNT_PER_PAGE * FLASH_RECORD_PAGES;
	
	preIndex = (WriteIndex + TotalCount - 1) % TotalCount;

	//计算地址
	addr = (preIndex / RECORD_COUNT_PER_PAGE) * 2048 + (preIndex % RECORD_COUNT_PER_PAGE) * RecordSize + FLASH_MARLIN_POWERPANIC + 4;
	FLASH_Unlock();
	FLASH_ProgramWord(addr, 0);
	FLASH_Lock();
}

 /**
 *Resume work after power panic if exist valid power panic data
 *return :true is resume success, or else false
 */
bool PowerPanic::PowerPanicResumeWork(uint8_t *Err)
{
	char tmpBuff[32];
	
	//暂存文件位置，运动会更新PowerPanicData  中的数据
	tmpPowerPanicData = Data;

	//数据有效
	if(tmpPowerPanicData.Valid != 0)
	{
		//U  盘打印断电
		if(tmpPowerPanicData.GCodeSource == GCODE_SOURCE_UDISK)
		{
      card.initsd();
      //尝试打开文件
      card.openFile(Data.FileName, true);
			
			//文件成功打开
			if(card.isFileOpen() == true)
			{
				//发送启动成功
				//HMI_SendPowerPanicResume(0x0c, 0);
				//风扇开启
				#ifdef H_FAN2_PORT
				H_FAN2_PORT->BSRR = H_FAN2_PIN;
				#endif
				//加热
				if(tmpPowerPanicData.BedTamp > 20)
				{
					sprintf(tmpBuff, "M140 S%0.2f", tmpPowerPanicData.BedTamp);
					process_cmd_imd(tmpBuff);
				}

				//目前只考虑加热头1
				sprintf(tmpBuff, "M104 S%0.2f", tmpPowerPanicData.HeaterTamp[0]);
				process_cmd_imd(tmpBuff);
				
				//X  Y  回原点
				strcpy(tmpBuff, "G28");
				process_cmd_imd(tmpBuff);
				
				//等待加热
				if(tmpPowerPanicData.BedTamp > 20)
				{
					sprintf(tmpBuff, "M190 S%0.2f", tmpPowerPanicData.BedTamp);
					process_cmd_imd(tmpBuff);
				}

				//目前只考虑加热头1
				sprintf(tmpBuff, "M109 S%0.2f", tmpPowerPanicData.HeaterTamp[0]);
				process_cmd_imd(tmpBuff);

				//绝对模式
				relative_mode = false;

				//移动到恢复点
				//调平失效
				set_bed_leveling_enabled(false);
				//移动到续点高度加5mm  位置
				sprintf(tmpBuff, "G0 Z%0.2f F2000", tmpPowerPanicData.PositionData[Z_AXIS] + 5);
				process_cmd_imd(tmpBuff);
				planner.synchronize();

				//预挤出 
				//相对模式
				relative_mode = true;
				strcpy(tmpBuff, "G0 E25 F100");
        process_cmd_imd(tmpBuff);
				planner.synchronize();
				//坐标更新
				current_position[E_AXIS] = tmpPowerPanicData.PositionData[E_AXIS];
        planner.set_e_position_mm(current_position[E_AXIS]);
				//绝对模式
				relative_mode = false;
				//暂停3  秒
				strcpy(tmpBuff, "G4 S3");
				process_cmd_imd(tmpBuff);
				//移动到续点XY  坐标
				sprintf(tmpBuff, "G0 X%0.2f Y%0.2f F4000", tmpPowerPanicData.PositionData[X_AXIS], tmpPowerPanicData.PositionData[Y_AXIS]);
				process_cmd_imd(tmpBuff);
				planner.synchronize();
				//移动到打印高度
				sprintf(tmpBuff, "G0 Z%0.2f F2000", tmpPowerPanicData.PositionData[Z_AXIS]);
				process_cmd_imd(tmpBuff);
				planner.synchronize();
				//计时器开始工作
				print_job_timer.start();
        Stopwatch::resume(tmpPowerPanicData.accumulator);

				//文件跳转
				card.setIndex(tmpPowerPanicData.FilePosition);
				//启动打印
				card.startFileprint();
				//绝对模式
				relative_mode = false;
				//同步坐标计数器
				//Sync_PCounter();

				//速度还原
				//SetG0FeedRate(Data.TravelFeedRate);
				//SetG1FeedRate(Data.PrintFeedRate);
				saved_g1_feedrate_mm_s = Data.PrintFeedRate;
        saved_g0_feedrate_mm_s = Data.TravelFeedRate;

				//调平使能
				set_bed_leveling_enabled(true);

				//Gcode  带Z  坐标
				if(tmpPowerPanicData.ZMove == 1)
				{
					
				}
				//Gcode  不带Z  坐标
				else
				{
					sprintf(tmpBuff, "G0 Z%0.2f F2000", tmpPowerPanicData.PositionData[Z_AXIS]);
					process_cmd_imd(tmpBuff);
					planner.synchronize();
				}
				
				*Err = 0;
        SystemStatus.SetCurrentPrinterStatus(STAT_RUNNING);
				//EnablePowerPanicCheck();
				if(MACHINE_TYPE_LASER == ExecuterHead.MachineType)
          Periph.StartDoorCheck();
				#if((HAVE_FILAMENT_SENSOR == 1) || (HAVE_FILAMENT_SWITCH == 1))
				if(MACHINE_TYPE_3DPRINT == ExecuterHead.MachineType)
					Periph.StartFilamentCheck();
				#endif
				//清除断电数据
				MaskPowerPanicData();
				return 0;
			}
			else
			{
				//发送失败
				//HMI_SendPowerPanicResume(0x0c, 1);
				if(Err != NULL)
					*Err = 1;
				return false;
			}
		}
		//联机打印断电
		else
		{
			//发送启动成功
			//HMI_SendPowerPanicResume(0x0b, 0);
			//风扇开启
			#ifdef H_FAN2_PORT
			H_FAN2_PORT->BSRR = H_FAN2_PIN;
			#endif
			//机型是3D  打印
			if(MACHINE_TYPE_3DPRINT == tmpPowerPanicData.MachineType)
			{
				//加热
				if(tmpPowerPanicData.BedTamp > 20)
				{
					sprintf(tmpBuff, "M140 S%0.2f", tmpPowerPanicData.BedTamp);
					process_cmd_imd(tmpBuff);
				}

				//目前只考虑加热头1
				sprintf(tmpBuff, "M104 S%0.2f", tmpPowerPanicData.HeaterTamp[0]);
				process_cmd_imd(tmpBuff);
			}
			
			//X  Y  回原点
			strcpy(tmpBuff, "G28");
			process_cmd_imd(tmpBuff);

			//机型是3D  打印
			if(MACHINE_TYPE_3DPRINT == tmpPowerPanicData.MachineType)
			{
				//等待加热
				if(tmpPowerPanicData.BedTamp > 20)
				{
					sprintf(tmpBuff, "M190 S%0.2f", tmpPowerPanicData.BedTamp);
					process_cmd_imd(tmpBuff);
				}

				//目前只考虑加热头1
				sprintf(tmpBuff, "M109 S%0.2f", tmpPowerPanicData.HeaterTamp[0]);
				process_cmd_imd(tmpBuff);
			}

			//绝对模式
			relative_mode = false;

			//移动到恢复点
			//调平失效
			set_bed_leveling_enabled(false);
			//移动到续点高度加5mm  位置
			sprintf(tmpBuff, "G0 Z%0.2f F2000", tmpPowerPanicData.PositionData[Z_AXIS] + 5);
			process_cmd_imd(tmpBuff);
			planner.synchronize();
			//机型是3D  打印
			if(MACHINE_TYPE_3DPRINT == tmpPowerPanicData.MachineType)
			{
				//预挤出 
				//相对模式
				relative_mode = true;
				strcpy(tmpBuff, "G0 E25 F100");
				process_cmd_imd(tmpBuff);
				planner.synchronize();
				//坐标更新
				current_position[E_AXIS] = tmpPowerPanicData.PositionData[E_AXIS];
				planner.set_e_position_mm(current_position[E_AXIS]);
			}
			//绝对模式
			relative_mode = false;
			//暂停3  秒
			strcpy(tmpBuff, "G4 S3");
			process_cmd_imd(tmpBuff);
			//移动到续点XY  坐标
			sprintf(tmpBuff, "G0 X%0.2f Y%0.2f F4000", tmpPowerPanicData.PositionData[X_AXIS], tmpPowerPanicData.PositionData[Y_AXIS]);
			process_cmd_imd(tmpBuff);
			planner.synchronize();
			//移动到打印高度
			sprintf(tmpBuff, "G0 Z%0.2f F2000", tmpPowerPanicData.PositionData[Z_AXIS]);
			process_cmd_imd(tmpBuff);
			planner.synchronize();

			//绝对模式
			relative_mode = false;
			//同步坐标计数器
			//Sync_PCounter();

			//速度还原
			saved_g1_feedrate_mm_s = Data.PrintFeedRate;
      saved_g0_feedrate_mm_s = Data.TravelFeedRate;

			//调平使能
			set_bed_leveling_enabled(true);

			//Gcode  带Z  坐标
			if(tmpPowerPanicData.ZMove == 1)
			{
				
			}
			//Gcode  不带Z  坐标
			else
			{
				sprintf(tmpBuff, "G0 Z%0.2f F2000", tmpPowerPanicData.PositionData[Z_AXIS]);
				process_cmd_imd(tmpBuff);
				planner.synchronize();
			}
			
			*Err = 0;
      SystemStatus.SetCurrentPrinterStatus(STAT_RUNNING_ONLINE);
			//EnablePowerPanicCheck();
			if(MACHINE_TYPE_LASER == ExecuterHead.MachineType)
				Periph.StartDoorCheck();
			#if((HAVE_FILAMENT_SENSOR == 1) || (HAVE_FILAMENT_SWITCH == 1))
			if(MACHINE_TYPE_3DPRINT == ExecuterHead.MachineType)
				Periph.StartFilamentCheck();
			#endif
			//清除断电数据
			MaskPowerPanicData();
			return 0;
		}
	}
	/*
	else
	{
		//发送失败
		HMI_SendPowerPanicResume(0x0c, 1);
		if(Err != NULL)
			*Err = 1;
		return false;
	}
	*/
    if(Err != NULL)
        *Err = 1;
    return false;
}